﻿using System.IO;

namespace Microwalk.TraceEntryTypes
{
    /// <summary>
    /// An access to memory allocated on the heap.
    /// </summary>
    public class HeapMemoryAccess : ITraceEntry
    {
        public TraceEntryTypes EntryType => TraceEntryTypes.HeapMemoryAccess;

        public void FromReader(FastBinaryReader reader)
        {
            IsWrite = reader.ReadBoolean();
            InstructionImageId = reader.ReadInt32();
            InstructionRelativeAddress = reader.ReadUInt32();
            MemoryAllocationBlockId = reader.ReadInt32();
            MemoryRelativeAddress = reader.ReadUInt32();
        }

        public void Store(BinaryWriter writer)
        {
            writer.Write((byte)TraceEntryTypes.HeapMemoryAccess);
            writer.Write(IsWrite);
            writer.Write(InstructionImageId);
            writer.Write(InstructionRelativeAddress);
            writer.Write(MemoryAllocationBlockId);
            writer.Write(MemoryRelativeAddress);
        }

        /// <summary>
        /// Determines whether this is a write access.
        /// </summary>
        public bool IsWrite { get; set; }

        /// <summary>
        /// The image ID of the accessing instruction.
        /// </summary>
        public int InstructionImageId { get; set; }

        /// <summary>
        /// The address of the accessing instruction, relative to the image start address.
        /// </summary>
        public uint InstructionRelativeAddress { get; set; }

        /// <summary>
        /// The allocation block ID of the accessed memory.
        /// </summary>
        public int MemoryAllocationBlockId { get; set; }

        /// <summary>
        /// The address of the accessed memory, relative to the allocated block's start address.
        /// </summary>
        public uint MemoryRelativeAddress { get; set; }
    }
}