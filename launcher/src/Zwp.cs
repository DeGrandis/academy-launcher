using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace AcademyLauncher
{
    // Clone Wars .zwp archives (data.zwp); the same format as tools/zwp/zwp.py.
    //   header (0x38 bytes): "NORK", u32 version (2), u32 0, u32 archive size, u32 entry count, u32 directory offset
    //   entry data packed back to back from 0x38: a zlib stream, or raw when compressing does not make it smaller
    //   directory: per entry u32 0, u8 name length, name, u32 data offset, u32 size, u32 stored size
    public static class Zwp
    {
        const int HeaderSize = 0x38;
        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        public sealed class Entry
        {
            public string Name;
            public long Offset;
            public int Size;
            public int StoredSize;
            public bool Compressed { get { return StoredSize != Size; } }
        }

        public static List<Entry> ReadDirectory(Stream stream)
        {
            var header = new byte[24];
            stream.Position = 0;
            ReadExactly(stream, header, header.Length);
            if (Encoding.ASCII.GetString(header, 0, 4) != "NORK" || BitConverter.ToUInt32(header, 4) != 2)
            {
                throw new InvalidDataException("data.zwp is not a version 2 NORK archive.");
            }
            uint total = BitConverter.ToUInt32(header, 12);
            int count = BitConverter.ToInt32(header, 16);
            uint directoryOffset = BitConverter.ToUInt32(header, 20);
            var directory = new byte[total - directoryOffset];
            stream.Position = directoryOffset;
            ReadExactly(stream, directory, directory.Length);
            var entries = new List<Entry>(count);
            int position = 0;
            for (int i = 0; i < count; i++)
            {
                int length = directory[position + 4];
                string name = Latin1.GetString(directory, position + 5, length);
                position += 5 + length;
                entries.Add(new Entry
                {
                    Name = name,
                    Offset = BitConverter.ToUInt32(directory, position),
                    Size = BitConverter.ToInt32(directory, position + 4),
                    StoredSize = BitConverter.ToInt32(directory, position + 8),
                });
                position += 12;
            }
            return entries;
        }

        public static byte[] ReadEntry(Stream stream, Entry entry)
        {
            var stored = new byte[entry.StoredSize];
            stream.Position = entry.Offset;
            ReadExactly(stream, stored, stored.Length);
            byte[] content = entry.Compressed ? Inflate(stored, entry.Size) : stored;
            if (content.Length != entry.Size)
            {
                throw new InvalidDataException(entry.Name + ": expected " + entry.Size + " bytes, got " + content.Length);
            }
            return content;
        }

        static void ReadExactly(Stream stream, byte[] buffer, int count)
        {
            int done = 0;
            while (done < count)
            {
                int read = stream.Read(buffer, done, count - done);
                if (read <= 0) throw new EndOfStreamException("data.zwp is truncated.");
                done += read;
            }
        }

        // zlib stream = 2-byte header, raw deflate, big-endian Adler-32 of the content.
        static byte[] Inflate(byte[] zlib, int size)
        {
            using (var input = new MemoryStream(zlib, 2, zlib.Length - 2))
            using (var inflate = new DeflateStream(input, CompressionMode.Decompress))
            {
                var content = new byte[size];
                int done = 0;
                while (done < size)
                {
                    int read = inflate.Read(content, done, size - done);
                    if (read <= 0) break;
                    done += read;
                }
                if (done != size) Array.Resize(ref content, done);
                return content;
            }
        }

        static byte[] Deflate(byte[] content)
        {
            using (var output = new MemoryStream())
            {
                output.WriteByte(0x78);
                output.WriteByte(0xDA);
                using (var deflate = new DeflateStream(output, CompressionLevel.Optimal, true))
                {
                    deflate.Write(content, 0, content.Length);
                }
                uint a = 1, b = 0;
                foreach (byte value in content)
                {
                    a = (a + value) % 65521;
                    b = (b + a) % 65521;
                }
                uint adler = (b << 16) | a;
                output.WriteByte((byte)(adler >> 24));
                output.WriteByte((byte)(adler >> 16));
                output.WriteByte((byte)(adler >> 8));
                output.WriteByte((byte)adler);
                return output.ToArray();
            }
        }

        // The game's rule: store compressed only when that is smaller.
        static byte[] Pack(byte[] content)
        {
            byte[] packed = Deflate(content);
            return packed.Length < content.Length ? packed : content;
        }

        // Writes `output`: every entry of `basePath` in order (unchanged ones copied without recompressing), entries named
        // in `overrides` (case-insensitive) replaced, then the remaining overrides appended in name order.
        public static void Build(string basePath, IDictionary<string, KeyValuePair<string, byte[]>> overrides, string output, Action<string> log)
        {
            var remaining = new Dictionary<string, KeyValuePair<string, byte[]>>(overrides, StringComparer.OrdinalIgnoreCase);
            var directory = new MemoryStream();
            var writer = new BinaryWriter(directory);
            int count = 0, replaced = 0;
            using (var source = new FileStream(basePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            using (var target = new FileStream(output, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                target.Write(new byte[HeaderSize], 0, HeaderSize);
                var buffer = new byte[1 << 20];
                Action<string, byte[], int> add = (name, stored, size) =>
                {
                    byte[] encoded = Latin1.GetBytes(name);
                    if (encoded.Length > 255) throw new InvalidDataException("name too long: " + name);
                    writer.Write(0u);
                    writer.Write((byte)encoded.Length);
                    writer.Write(encoded);
                    writer.Write((uint)target.Position);
                    writer.Write(size);
                    writer.Write(stored.Length);
                    target.Write(stored, 0, stored.Length);
                    count++;
                };
                foreach (Entry entry in ReadDirectory(source))
                {
                    KeyValuePair<string, byte[]> replacement;
                    if (remaining.TryGetValue(entry.Name, out replacement))
                    {
                        remaining.Remove(entry.Name);
                        add(entry.Name, Pack(replacement.Value), replacement.Value.Length);
                        replaced++;
                        continue;
                    }
                    byte[] latin = Latin1.GetBytes(entry.Name);
                    writer.Write(0u);
                    writer.Write((byte)latin.Length);
                    writer.Write(latin);
                    writer.Write((uint)target.Position);
                    writer.Write(entry.Size);
                    writer.Write(entry.StoredSize);
                    source.Position = entry.Offset;
                    int left = entry.StoredSize;
                    while (left > 0)
                    {
                        int read = source.Read(buffer, 0, Math.Min(buffer.Length, left));
                        if (read <= 0) throw new EndOfStreamException("data.zwp is truncated.");
                        target.Write(buffer, 0, read);
                        left -= read;
                    }
                    count++;
                }
                var added = new List<string>(remaining.Keys);
                added.Sort(StringComparer.Ordinal);
                foreach (string key in added)
                {
                    KeyValuePair<string, byte[]> file = remaining[key];
                    add(file.Key, Pack(file.Value), file.Value.Length);
                }
                long directoryOffset = target.Position;
                writer.Flush();
                directory.WriteTo(target);
                long total = target.Position;
                if (total > uint.MaxValue) throw new InvalidDataException("data.zwp would be larger than 4 GB.");
                var header = new byte[HeaderSize];
                Encoding.ASCII.GetBytes("NORK").CopyTo(header, 0);
                BitConverter.GetBytes(2u).CopyTo(header, 4);
                BitConverter.GetBytes((uint)total).CopyTo(header, 12);
                BitConverter.GetBytes(count).CopyTo(header, 16);
                BitConverter.GetBytes((uint)directoryOffset).CopyTo(header, 20);
                target.Position = 0;
                target.Write(header, 0, header.Length);
                log("data.zwp: " + count + " entries, " + replaced + " replaced, " + added.Count + " added");
            }
        }
    }
}
