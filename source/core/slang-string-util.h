#ifndef SLANG_CORE_STRING_UTIL_H
#define SLANG_CORE_STRING_UTIL_H

#include "slang-string.h"
#include "slang-list.h"

#include <stdarg.h>

#include "../../slang-com-helper.h"
#include "../../slang-com-ptr.h"

namespace Slang {

/** A blob that uses a `String` for its storage.
*/
class StringBlob : public ISlangBlob, public RefObject
{
public:
    // ISlangUnknown
    SLANG_REF_OBJECT_IUNKNOWN_ALL

        // ISlangBlob
    SLANG_NO_THROW void const* SLANG_MCALL getBufferPointer() SLANG_OVERRIDE { return m_string.getBuffer(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() SLANG_OVERRIDE { return m_string.getLength(); }

        /// Get the contained string
    SLANG_FORCE_INLINE const String& getString() const { return m_string; }

    explicit StringBlob(String const& string)
        : m_string(string)
    {}

protected:
    ISlangUnknown* getInterface(const Guid& guid);
    String m_string;
};

struct StringUtil
{
        /// Split in, by specified splitChar into slices out
        /// Slices contents will directly address into in, so contents will only stay valid as long as in does.
    static void split(const UnownedStringSlice& in, char splitChar, List<UnownedStringSlice>& slicesOut);

        /// Equivalent to doing a split and then finding the index of 'find' on the array
        /// Returns -1 if not found
    static int indexOfInSplit(const UnownedStringSlice& in, char splitChar, const UnownedStringSlice& find);

        /// Given the entry at the split index specified.
        /// Will return slice with begin() == nullptr if not found or input has begin() == nullptr)
    static UnownedStringSlice getAtInSplit(const UnownedStringSlice& in, char splitChar, int index);

        /// Returns the size in bytes needed to hold the formatted string using the specified args, NOT including a terminating 0
        /// NOTE! The caller *should* assume this will consume the va_list (use va_copy to make a copy to be consumed)
    static size_t calcFormattedSize(const char* format, va_list args);

        /// Calculate the formatted string using the specified args.
        /// NOTE! The caller *should* assume this will consume the va_list
        /// The buffer should be at least calcFormattedSize + 1 bytes. The +1 is needed because a terminating 0 is written. 
    static void calcFormatted(const char* format, va_list args, size_t numChars, char* dst);

        /// Appends formatted string with args into buf
    static void append(const char* format, va_list args, StringBuilder& buf);

        /// Appends the formatted string with specified trailing args
    static void appendFormat(StringBuilder& buf, const char* format, ...);

        /// Create a string from the format string applying args (like sprintf)
    static String makeStringWithFormat(const char* format, ...);

        /// Given a string held in a blob, returns as a String
        /// Returns an empty string if blob is nullptr 
    static String getString(ISlangBlob* blob);

        /// Given a string or slice, replaces all instances of fromChar with toChar
    static String calcCharReplaced(const UnownedStringSlice& slice, char fromChar, char toChar);
    static String calcCharReplaced(const String& string, char fromChar, char toChar);
    
        /// Create a blob from a string
    static ComPtr<ISlangBlob> createStringBlob(const String& string);
};

} // namespace Slang

#endif // SLANG_STRING_UTIL_H
