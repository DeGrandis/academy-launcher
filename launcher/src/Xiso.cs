using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace AcademyLauncher
{
    // Reads Xbox game disc images (XDVDFS). Accepts a plain XISO and full disc dumps (redump), where the game partition
    // starts at a fixed offset after the video partition.
    //
    // Volume descriptor (sector 32 of the partition): "MICROSOFT*XBOX*MEDIA", u32 root directory sector, u32 root
    // directory size. A directory is a binary tree of entries, each 4-byte aligned and never crossing a sector:
    // u16 left child, u16 right child (offsets in dwords; 0 = none, 0xFFFF = padding), u32 start sector, u32 size,
    // u8 attributes (0x10 = directory), u8 name length, name.
    public sealed class Xiso : IDisposable
    {
        const int SectorSize = 2048;
        static readonly long[] PartitionOffsets = { 0, 0x18300000, 0xFD90000, 0x2080000 };
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

        public sealed class Entry
        {
            public string Path;  // relative, backslash separated
            public long Offset;  // absolute in the image
            public long Size;
            public bool IsDirectory;
        }

        readonly FileStream stream;
        readonly long partition;

        public Xiso(string imagePath)
        {
            stream = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            partition = -1;
            foreach (long offset in PartitionOffsets)
            {
                if (offset + 0x10000 + Magic.Length <= stream.Length && Matches(Read(offset + 0x10000, Magic.Length), Magic))
                {
                    partition = offset;
                    break;
                }
            }
            if (partition < 0)
            {
                stream.Dispose();
                throw new InvalidDataException("This file is not an Xbox game disc image (no XDVDFS volume found).");
            }
        }

        public void Dispose()
        {
            stream.Dispose();
        }

        static bool Matches(byte[] data, byte[] expected)
        {
            for (int i = 0; i < expected.Length; i++)
            {
                if (data[i] != expected[i]) return false;
            }
            return true;
        }

        byte[] Read(long offset, int count)
        {
            var buffer = new byte[count];
            stream.Position = offset;
            int done = 0;
            while (done < count)
            {
                int read = stream.Read(buffer, done, count - done);
                if (read <= 0) throw new EndOfStreamException("The disc image is truncated.");
                done += read;
            }
            return buffer;
        }

        public List<Entry> ListFiles()
        {
            byte[] volume = Read(partition + 0x10000, 32);
            uint rootSector = BitConverter.ToUInt32(volume, 20);
            uint rootSize = BitConverter.ToUInt32(volume, 24);
            var entries = new List<Entry>();
            ReadDirectory(rootSector, rootSize, "", entries, 0);
            return entries;
        }

        void ReadDirectory(uint sector, uint size, string prefix, List<Entry> entries, int depth)
        {
            if (size == 0 || depth > 32)
            {
                return;
            }
            byte[] table = Read(partition + (long)sector * SectorSize, (int)size);
            var pending = new Stack<int>();
            var visited = new HashSet<int>();
            pending.Push(0);
            var found = new List<Entry>();
            while (pending.Count > 0)
            {
                int offset = pending.Pop();
                if (offset + 14 > table.Length || !visited.Add(offset))
                {
                    continue;
                }
                ushort left = BitConverter.ToUInt16(table, offset);
                ushort right = BitConverter.ToUInt16(table, offset + 2);
                if (left == 0xFFFF && right == 0xFFFF)
                {
                    continue;
                }
                uint start = BitConverter.ToUInt32(table, offset + 4);
                uint length = BitConverter.ToUInt32(table, offset + 8);
                byte attributes = table[offset + 12];
                int nameLength = table[offset + 13];
                if (offset + 14 + nameLength > table.Length)
                {
                    continue;
                }
                string name = Encoding.GetEncoding(28591).GetString(table, offset + 14, nameLength);
                if (name.Length == 0 || name.IndexOfAny(System.IO.Path.GetInvalidFileNameChars()) >= 0 || name == "." || name == "..")
                {
                    throw new InvalidDataException("The disc image has an invalid file name.");
                }
                found.Add(new Entry
                {
                    Path = prefix + name,
                    Offset = partition + (long)start * SectorSize,
                    Size = length,
                    IsDirectory = (attributes & 0x10) != 0,
                });
                if (left != 0 && left != 0xFFFF) pending.Push(left * 4);
                if (right != 0 && right != 0xFFFF) pending.Push(right * 4);
                if ((attributes & 0x10) != 0)
                {
                    ReadDirectory(start, length, prefix + name + "\\", entries, depth + 1);
                }
            }
            entries.AddRange(found);
        }

        // Copies every file into `target`. Reports (bytes done, bytes total).
        public void ExtractAll(string target, Action<long, long> progress, CancellationToken cancel)
        {
            List<Entry> entries = ListFiles();
            long total = 0;
            foreach (Entry entry in entries)
            {
                if (!entry.IsDirectory) total += entry.Size;
            }
            long done = 0;
            var buffer = new byte[4 << 20];
            foreach (Entry entry in entries)
            {
                string path = System.IO.Path.Combine(target, entry.Path);
                if (entry.IsDirectory)
                {
                    Directory.CreateDirectory(path);
                    continue;
                }
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path));
                using (var output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                {
                    stream.Position = entry.Offset;
                    long remaining = entry.Size;
                    while (remaining > 0)
                    {
                        cancel.ThrowIfCancellationRequested();
                        int read = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                        if (read <= 0) throw new EndOfStreamException("The disc image is truncated.");
                        output.Write(buffer, 0, read);
                        remaining -= read;
                        done += read;
                        progress(done, total);
                    }
                }
            }
        }
    }
}
