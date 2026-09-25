/* Target character classification for MS-DOS64.
 *
 * The classification is locale-free ASCII, which is the only character set the
 * target contract defines. Every routine takes an int so that the usual
 * `(unsigned char)` cast at the call site is meaningful. */

#include <ctype.h>

int isdigit(int character)
{
    return character >= '0' && character <= '9';
}

int isxdigit(int character)
{
    if (character >= '0' && character <= '9') return 1;
    if (character >= 'a' && character <= 'f') return 1;
    if (character >= 'A' && character <= 'F') return 1;
    return 0;
}

int islower(int character)
{
    return character >= 'a' && character <= 'z';
}

int isupper(int character)
{
    return character >= 'A' && character <= 'Z';
}

int isalpha(int character)
{
    return islower(character) || isupper(character);
}

int isalnum(int character)
{
    return isalpha(character) || isdigit(character);
}

int isspace(int character)
{
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\v' || character == '\f';
}

int isblank(int character)
{
    return character == ' ' || character == '\t';
}

int iscntrl(int character)
{
    return (character >= 0 && character < 32) || character == 127;
}

int isprint(int character)
{
    return character >= 32 && character < 127;
}

int isgraph(int character)
{
    return character > 32 && character < 127;
}

int ispunct(int character)
{
    return isgraph(character) && !isalnum(character);
}

int tolower(int character)
{
    return isupper(character) ? character + 32 : character;
}

int toupper(int character)
{
    return islower(character) ? character - 32 : character;
}
