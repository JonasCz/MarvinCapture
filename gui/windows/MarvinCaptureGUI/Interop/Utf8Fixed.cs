using System;
using System.Text;

namespace PinnacleCapture.Interop;

/// <summary>
/// pin_api.h embeds UTF-8 C strings as fixed-size char[] arrays inside
/// structs (PIN_NAME_MAX/PIN_PATH_MAX/PIN_TEXT_MAX and the smaller ad-hoc
/// buffers like timecode[16]) rather than pointers, so they round-trip by
/// value with the rest of the struct. Those are not marshallable as a
/// managed `string` by the source-generated P/Invoke, so each struct below
/// declares them as `fixed byte` buffers and uses these helpers to encode
/// and decode them by hand.
/// </summary>
internal static unsafe class Utf8Fixed
{
    /// <summary>Reads a NUL-terminated UTF-8 string out of a fixed buffer.</summary>
    public static string Get(byte* buffer, int capacity)
    {
        int len = 0;
        while (len < capacity && buffer[len] != 0)
        {
            len++;
        }
        return len == 0 ? string.Empty : Encoding.UTF8.GetString(buffer, len);
    }

    /// <summary>
    /// Writes a string into a fixed buffer as UTF-8, NUL-terminated,
    /// truncating (on a whole-byte boundary) if it doesn't fit.
    /// </summary>
    public static void Set(byte* buffer, int capacity, string? value)
    {
        for (int i = 0; i < capacity; i++)
        {
            buffer[i] = 0;
        }
        if (string.IsNullOrEmpty(value) || capacity <= 0)
        {
            return;
        }

        int maxBytes = capacity - 1; // room for the terminator
        byte[] encoded = Encoding.UTF8.GetBytes(value);
        int copy = Math.Min(maxBytes, encoded.Length);

        // Avoid splitting a multi-byte UTF-8 sequence in half (only relevant
        // when we actually truncated; a full, untruncated copy is already safe).
        if (copy < encoded.Length)
        {
            while (copy > 0 && (encoded[copy] & 0xC0) == 0x80)
            {
                copy--;
            }
        }

        for (int i = 0; i < copy; i++)
        {
            buffer[i] = encoded[i];
        }
        buffer[copy] = 0;
    }
}
