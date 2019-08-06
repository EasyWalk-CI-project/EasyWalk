/*
IMPORTANT: The instrumented program or one of its dependencies MUST contain (named) "malloc" and "free" functions.
To get meaningful outputs, make sure that these functions are called with "call" and have a "ret" instruction (no "jmp" to another function).
*/


/* INCLUDES */
#include "TraceWriter.h"
#include <xed-interface.h>
#include "Utilities.h"
#include "CpuOverride.h"


/* GLOBAL VARIABLES */

// The output file command line option.
KNOB<string> KnobOutputFilePrefix(KNOB_MODE_WRITEONCE, "pintool", "o", "out", "specify file name/path prefix for LeakageDetectorTrace output");

// The names of interesting images, separated by semicolons.
KNOB<string> KnobInterestingImageList(KNOB_MODE_WRITEONCE, "pintool", "i", ".exe", "specify list of interesting images, separated by semicolons");

// The desired CPU feature level.
KNOB<int> KnobCpuFeatureLevel(KNOB_MODE_WRITEONCE, "pintool", "c", "0", "specify desired CPU model: 0 = Default, 1 = Pentium3, 2 = Merom, 3 = Westmere, 4 = Ivybridge (your own CPU should form a superset of the selected option)");

// Constant random number generator value.
// Magic default value is 0xBADBADBADBADBAD (Pin does not provide an API to check whether parameter is actually in the command line).
KNOB<unsigned long long> KnobFixedRandomNumbers(KNOB_MODE_WRITEONCE, "pintool", "r", "841534158063459245", "set constant output for RDRAND instruction");

// The names of interesting images, parsed from the command line option.
vector<string> _interestingImages;

// The thread local storage key for the trace logger objects.
TLS_KEY _traceWriterTlsKey;

// The next writable entry buffer position (per thread).
REG _nextBufferEntryReg;

// The end of the entry buffer (per thread).
REG _entryBufferEndReg;

// The EAX input register of a CPUID instruction.
REG _cpuIdEaxInputReg;

// The ECX input register of a CPUID instruction.
REG _cpuIdEcxInputReg;

// Data of loaded images for lookup during trace instrumentation.
vector<ImageData*> _images;

// Determines whether RDRAND random numbers shall be replaced by fixed ones.
bool _useFixedRandomNumber = false;

// The fixed random number to be returned after each RDRAND instruction.
unsigned long long _fixedRandomNumber = 0;


/* CALLBACK PROTOTYPES */

VOID InstrumentTrace(TRACE trace, VOID* v);
VOID ThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v);
VOID ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v);
VOID InstrumentImage(IMG img, VOID* v);
TraceEntry* CheckBufferAndStore(TraceEntry* nextEntry, TraceEntry* entryBufferEnd, THREADID tid);
TraceEntry* TestcaseStart(ADDRINT newTestcaseId, THREADID tid, TraceEntry* nextEntry);
TraceEntry* TestcaseEnd(TraceEntry* nextEntry, THREADID tid);
EXCEPT_HANDLING_RESULT HandlePinToolException(THREADID tid, EXCEPTION_INFO* exceptionInfo, PHYSICAL_CONTEXT* physicalContext, VOID* v);
ADDRINT CheckNextTraceEntryPointerValid(TraceEntry* nextEntry);
void ChangeRandomNumber(ADDRINT* outputReg);
void ChangeCpuId(UINT32 inputEax, UINT32 inputEcx, UINT32* outputEax, UINT32* outputEbx, UINT32* outputEcx, UINT32* outputEdx);


/* FUNCTIONS */

// The main procedure of the tool.
int main(int argc, char* argv[])
{
    // Initialize PIN library
    if(PIN_Init(argc, argv))
    {
        // Print help message if -h(elp) is specified in the command line or the command line is invalid 
        cerr << KNOB_BASE::StringKnobSummary() << endl;
        return -1;
    }
    
    // Split list of interesting images
    stringstream interestingImagesStringStream(KnobInterestingImageList);
    string item;
    while(getline(interestingImagesStringStream, item, ';'))
        if(!item.empty())
        {
            tolower(item);
            _interestingImages.push_back(item);
        }

    // Create trace entry buffer and all associated variables
    _traceWriterTlsKey = PIN_CreateThreadDataKey(0);
    _nextBufferEntryReg = PIN_ClaimToolRegister();
    _entryBufferEndReg = PIN_ClaimToolRegister();

    // Reserve tool registers for CPUID modification
    _cpuIdEaxInputReg = PIN_ClaimToolRegister();
    _cpuIdEcxInputReg = PIN_ClaimToolRegister();

    // Set model for CPU emulation
    SetEmulatedCpu(KnobCpuFeatureLevel.Value());

    // Check if constant random numbers are desired
    if(KnobFixedRandomNumbers.Value() != 0xBADBADBADBADBAD)
    {
        _useFixedRandomNumber = true;
        _fixedRandomNumber = KnobFixedRandomNumbers.Value();
        cerr << "Using fixed RDRAND output " << _fixedRandomNumber << endl;
    }

    // Initialize prefix mode
    TraceWriter::InitPrefixMode(KnobOutputFilePrefix.Value());

    // Instrument instructions and routines
    IMG_AddInstrumentFunction(InstrumentImage, 0);
    TRACE_AddInstrumentFunction(InstrumentTrace, 0);

    // Set thread event handlers
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);

    // Handle internal exceptions (for debugging)
    PIN_AddInternalExceptionHandler(HandlePinToolException, NULL);

    // Load symbols to access function name information
    PIN_InitSymbols();

    // Start the target program
    PIN_StartProgram();
    return 0;
}


/* CALLBACKS */

// [Callback] Instruments memory access instructions.
VOID InstrumentTrace(TRACE trace, VOID* v)
{
    // Check each instruction in each basic block
    for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Before instrumentation check first whether we are in an interesting image
        // TODO this skips branches from uninteresting images to interesting images -> relevant?
        ImageData* img = nullptr;
        for(ImageData* i : _images)
            if(i->ContainsBasicBlock(bbl))
            {
                img = i;
                break;
            }
        if(img == nullptr)
        {
            // Should not happen
            cerr << "Error: Cannot resolve image of basic block " << hex << BBL_Address(bbl) << endl;
            continue;
        }
        bool interesting = img->IsInteresting();

        // Run through instructions
        for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
            // Ignore everything that uses segment registers (shouldn't be used by modern software, except in a few cases by operating systems)
            // Windows e.g. uses GS for thread local storage
            // TODO Hint that in documentation
            if(INS_SegmentPrefix(ins))
                continue;

            // Ignore some frequent and uninteresting instructions to reduce overhead
            OPCODE opc = INS_Opcode(ins);
            if(opc >= XED_ICLASS_PUSH && opc <= XED_ICLASS_PUSHFQ)
                continue;
            if(opc >= XED_ICLASS_POP && opc <= XED_ICLASS_POPFQ)
                continue;
            if(opc == XED_ICLASS_LEA)
                continue;

            // Change CPUID instruction
            if(opc == XED_ICLASS_CPUID)
            {
                // Save input registers
                INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(PIN_SetContextReg),
                    IARG_CONTEXT,
                    IARG_UINT32, _cpuIdEaxInputReg,
                    IARG_REG_VALUE, REG_EAX,
                    IARG_END);
                INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(PIN_SetContextReg),
                    IARG_CONTEXT,
                    IARG_UINT32, _cpuIdEcxInputReg,
                    IARG_REG_VALUE, REG_ECX,
                    IARG_END);

                // Modify output registers
                INS_InsertCall(ins, IPOINT_AFTER, AFUNPTR(ChangeCpuId),
                    IARG_REG_VALUE, _cpuIdEaxInputReg,
                    IARG_REG_VALUE, _cpuIdEcxInputReg,
                    IARG_REG_REFERENCE, REG_EAX,
                    IARG_REG_REFERENCE, REG_EBX,
                    IARG_REG_REFERENCE, REG_ECX,
                    IARG_REG_REFERENCE, REG_EDX,
                    IARG_END);
                continue;
            }

            // Change RDRAND instruction
            if(opc == XED_ICLASS_RDRAND && _useFixedRandomNumber)
            {
                // Modify output register
                INS_InsertCall(ins, IPOINT_AFTER, AFUNPTR(ChangeRandomNumber),
                    IARG_REG_REFERENCE, INS_RegW(ins, 0),
                    IARG_END);
                continue;
            }

            // Trace branch instructions (conditional and unconditional)
            if(INS_IsCall(ins))
            {
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertBranchEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_BRANCH_TARGET_ADDR,
                    IARG_BOOL, 1,
                    IARG_UINT32, 1,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                continue;
            }
            if(INS_IsBranch(ins))
            {
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertBranchEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_BRANCH_TARGET_ADDR,
                    IARG_BRANCH_TAKEN,
                    IARG_UINT32, 0,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                continue;
            }
            if(INS_IsRet(ins))
            {
                // ret instructions cannot be instrumented with IPOINT_AFTER, since they do have no fallthrough
                INS_InsertIfCall(ins, IPOINT_TAKEN_BRANCH, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_TAKEN_BRANCH, AFUNPTR(TraceWriter::InsertRetBranchEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_CONTEXT,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_TAKEN_BRANCH, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_TAKEN_BRANCH, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
            }

            // Ignore everything else in uninteresting images
            if(!interesting)
                continue;

            // Trace instructions with memory read
            if(INS_IsMemoryRead(ins) && INS_IsStandardMemop(ins))
            {
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertMemoryReadEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_MEMORYREAD_EA,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
            }

            // Trace instructions with a second memory read operand
            if(INS_HasMemoryRead2(ins) && INS_IsStandardMemop(ins))
            {
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertMemoryReadEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_MEMORYREAD2_EA,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
            }

            // Trace instructions with memory write
            if(INS_IsMemoryWrite(ins) && INS_IsStandardMemop(ins))
            {
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(CheckNextTraceEntryPointerValid),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertMemoryWriteEntry),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_INST_PTR,
                    IARG_MEMORYWRITE_EA,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
                INS_InsertIfCall(ins, IPOINT_BEFORE, AFUNPTR(TraceWriter::CheckBufferFull),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_END);
                INS_InsertThenCall(ins, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
                    IARG_REG_VALUE, _nextBufferEntryReg,
                    IARG_REG_VALUE, _entryBufferEndReg,
                    IARG_THREAD_ID,
                    IARG_RETURN_REGS, _nextBufferEntryReg,
                    IARG_END);
            }
        }
    }
}

// [Callback] Creates a new trace logger for the given new thread.
VOID ThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v)
{
    // Only instrument main thread
    if(tid == 0)
    {
        // Create new trace logger for this thread
        TraceWriter* traceWriter = new TraceWriter(KnobOutputFilePrefix.Value());

        // Put logger into local storage of this thread
        PIN_SetThreadData(_traceWriterTlsKey, traceWriter, tid);

        // Initialize entry buffer pointers
        PIN_SetContextReg(ctxt, _nextBufferEntryReg, reinterpret_cast<ADDRINT>(traceWriter->Begin()));
        PIN_SetContextReg(ctxt, _entryBufferEndReg, reinterpret_cast<ADDRINT>(traceWriter->End()));
    }
    else
    {
        // Set entry buffer pointers as null pointers
        cerr << "Ignoring thread #" << tid << endl;
        PIN_SetContextReg(ctxt, _nextBufferEntryReg, NULL);
        PIN_SetContextReg(ctxt, _entryBufferEndReg, NULL);
    }
}

// [Callback] Cleans up after thread exit.
VOID ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v)
{
    // Only the main thread is instrumented
    if(tid != 0)
        return;

    // Finalize trace logger of this thread
    TraceWriter* traceWriter = static_cast<TraceWriter*>(PIN_GetThreadData(_traceWriterTlsKey, tid));
    traceWriter->WriteBufferToFile(reinterpret_cast<TraceEntry*>(PIN_GetContextReg(ctxt, _nextBufferEntryReg)));
    delete traceWriter;
    PIN_SetThreadData(_traceWriterTlsKey, nullptr, tid);
}

// [Callback] Instruments the memory allocation/deallocation functions.
// TODO instrument malloc() and free() non Windows-specific
VOID InstrumentImage(IMG img, VOID* v)
{
    // Retrieve image name
    string imageName = IMG_Name(img);

    // Check whether image is interesting (its name appears in the image name list passed over the command line)
    string imageNameLower = imageName;
    tolower(imageNameLower);
    INT8 interesting = (find_if(_interestingImages.begin(), _interestingImages.end(), [&](string& interestingImageName) { return imageNameLower.find(interestingImageName) != string::npos; }) != _interestingImages.end()) ? 1 : 0;

    // Retrieve image memory offsets
    UINT64 imageStart = IMG_LowAddress(img);
    UINT64 imageEnd = IMG_HighAddress(img);

    // Record image data
    TraceWriter::WriteImageLoadData(static_cast<int>(interesting), imageStart, imageEnd, imageName);

    // Remember image for filtered trace instrumentation
    _images.push_back(new ImageData(interesting, imageName, imageStart, imageEnd));
    cerr << "Image '" << imageName << "' loaded at " << hex << imageStart << " ... " << hex << imageEnd << (interesting != 0 ? " [interesting]" : "") << endl;

    // Find the Pin notification functions to insert testcase markers
    RTN notifyStartRtn = RTN_FindByName(img, "PinNotifyTestcaseStart");
    if(RTN_Valid(notifyStartRtn))
    {
        // Switch to next testcase
        RTN_Open(notifyStartRtn);
        RTN_InsertCall(notifyStartRtn, IPOINT_BEFORE, AFUNPTR(TestcaseStart),
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_THREAD_ID,
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_Close(notifyStartRtn);

        cerr << "    PinNotifyTestcaseStart() instrumented." << endl;
    }
    RTN notifyEndRtn = RTN_FindByName(img, "PinNotifyTestcaseEnd");
    if(RTN_Valid(notifyEndRtn))
    {
        // Close testcase
        RTN_Open(notifyEndRtn);
        RTN_InsertCall(notifyEndRtn, IPOINT_BEFORE, AFUNPTR(TestcaseEnd),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_Close(notifyEndRtn);

        cerr << "    PinNotifyTestcaseEnd() instrumented." << endl;
    }

    // Find the Pin stack pointer notification function
    RTN notifyStackPointerRtn = RTN_FindByName(img, "PinNotifyStackPointer");
    if(RTN_Valid(notifyStackPointerRtn))
    {
        // Save stack pointer value
        RTN_Open(notifyStackPointerRtn);
        // Min
        RTN_InsertCall(notifyStackPointerRtn, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertStackPointerWriteEntry),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_InsertCall(notifyStackPointerRtn, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, _entryBufferEndReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        // Max
        RTN_InsertCall(notifyStackPointerRtn, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertStackPointerWriteEntry),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_InsertCall(notifyStackPointerRtn, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, _entryBufferEndReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_Close(notifyStackPointerRtn);

        cerr << "    PinNotifyStackPointer() instrumented." << endl;
    }

    // Find the malloc() function to log allocation sizes and addresses    
    RTN mallocRtn = RTN_FindByName(img, "RtlAllocateHeap");
    if(RTN_Valid(mallocRtn))
    {
        // Trace size parameter
        RTN_Open(mallocRtn);
        RTN_InsertCall(mallocRtn, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertAllocSizeParameterEntry),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_InsertCall(mallocRtn, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, _entryBufferEndReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);

        // Trace returned address
        RTN_InsertCall(mallocRtn, IPOINT_AFTER, AFUNPTR(TraceWriter::InsertAllocAddressReturnEntry),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, REG_RAX,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_InsertCall(mallocRtn, IPOINT_AFTER, AFUNPTR(CheckBufferAndStore),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, _entryBufferEndReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_Close(mallocRtn);

        cerr << "    malloc() instrumented." << endl;
    }

    // Find the free() function to log free addresses    
    RTN freeRtn = RTN_FindByName(img, "RtlFreeHeap");
    if(RTN_Valid(freeRtn))
    {
        // Trace address parameter
        RTN_Open(freeRtn);
        RTN_InsertCall(freeRtn, IPOINT_BEFORE, AFUNPTR(TraceWriter::InsertFreeAddressParameterEntry),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_InsertCall(freeRtn, IPOINT_BEFORE, AFUNPTR(CheckBufferAndStore),
            IARG_REG_VALUE, _nextBufferEntryReg,
            IARG_REG_VALUE, _entryBufferEndReg,
            IARG_THREAD_ID,
            IARG_RETURN_REGS, _nextBufferEntryReg,
            IARG_END);
        RTN_Close(freeRtn);

        cerr << "    free() instrumented." << endl;
    }
}

// Determines whether the given trace entry buffer is full, and stores its contents if neccessary.
TraceEntry* CheckBufferAndStore(TraceEntry* nextEntry, TraceEntry* entryBufferEnd, THREADID tid)
{
    // Only the main thread is instrumented
    if(tid != 0 || nextEntry == NULL || entryBufferEnd == NULL)
        return nextEntry;

    // Buffer full?
    if(TraceWriter::CheckBufferFull(nextEntry, entryBufferEnd))
    {
        // Get trace logger object and store contents
        TraceWriter* traceWriter = static_cast<TraceWriter*>(PIN_GetThreadData(_traceWriterTlsKey, tid));
        traceWriter->WriteBufferToFile(entryBufferEnd);
        return traceWriter->Begin();
    }
    return nextEntry;
}

// Handles the beginning of a testcase.
TraceEntry* TestcaseStart(ADDRINT newTestcaseId, THREADID tid, TraceEntry* nextEntry)
{
    // Get trace logger object and set the new testcase ID
    TraceWriter* traceWriter = static_cast<TraceWriter*>(PIN_GetThreadData(_traceWriterTlsKey, tid));
    traceWriter->TestcaseStart(static_cast<int>(newTestcaseId), nextEntry);
    return traceWriter->Begin();
}

// Handles the ending of a testcase.
TraceEntry* TestcaseEnd(TraceEntry* nextEntry, THREADID tid)
{
    // Get trace logger object and set the new testcase ID
    TraceWriter* traceWriter = static_cast<TraceWriter*>(PIN_GetThreadData(_traceWriterTlsKey, tid));
    traceWriter->TestcaseEnd(nextEntry);
    return traceWriter->Begin();
}

// Handles an internal exception of this trace tool.
EXCEPT_HANDLING_RESULT HandlePinToolException(THREADID tid, EXCEPTION_INFO* exceptionInfo, PHYSICAL_CONTEXT* physicalContext, VOID* v)
{
    // Output exception data
    std::cerr << "Internal exception: " << PIN_ExceptionToString(exceptionInfo) << endl;
    return EHR_UNHANDLED;
}

// Converts the given trace entry pointer into its address integer (which is then checked for NULL by Pin).
ADDRINT CheckNextTraceEntryPointerValid(TraceEntry* nextEntry)
{
    return reinterpret_cast<ADDRINT>(nextEntry);
}

// Overwrites the given destination register of the RDRAND instruction with a constant value.
void ChangeRandomNumber(ADDRINT* outputReg)
{
    *outputReg = static_cast<ADDRINT>(_fixedRandomNumber);
}