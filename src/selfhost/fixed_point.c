/* Fixed-point input for the self-host check.
 *
 * The target-built compiler compiles this file and the linker it contains
 * produces the image that then runs, so the file has to exercise enough of the
 * language that a difference between the host-built compiler and the
 * self-hosted one shows up in the object bytes rather than hiding behind an
 * unused feature. Every construct here is part of the documented C17 subset in
 * docs/specs/c-subset-v1.md.
 */

typedef unsigned long size_type;

struct Point {
    int x;
    int y;
};

union Word {
    unsigned long whole;
    unsigned char byte[4];
};

enum Kind { KIND_ZERO, KIND_ONE, KIND_MANY = 7 };

static int total;

static void advance(struct Point *point, int step)
{
    point->x += step;
    point->y -= step;
}

static unsigned long measure(const char *text)
{
    unsigned long length = 0UL;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

static int classify(enum Kind kind)
{
    int result;
    switch (kind) {
    case KIND_ZERO: result = 10; break;
    case KIND_ONE: result = 20; break;
    case KIND_MANY: result = 30; break;
    default: result = 40; break;
    }
    return result;
}

static unsigned long mix(unsigned long value)
{
    unsigned long accumulator = 0UL;
    int index;
    for (index = 0; index < 8; ++index) {
        accumulator = accumulator * 3UL + (unsigned long)index;
    }
    return accumulator ^ value;
}

int main(void)
{
    struct Point point;
    union Word word;
    int values[5];
    int index;
    int running = 0;

    point.x = 3;
    point.y = 4;
    advance(&point, 2);
    running += point.x * 10 + point.y;

    word.whole = 0UL;
    word.byte[0] = 65;
    word.byte[1] = 66;
    running += (int)(word.byte[0] + word.byte[1]);

    for (index = 0; index < 5; ++index) {
        values[index] = index * index;
    }
    index = 0;
    do {
        running += values[index];
        ++index;
    } while (index < 5);

    running += classify(KIND_MANY);
    running += (int)measure("abcdefgh");
    running += (int)(mix(11UL) & 15UL);
    total = running;
    return total % 100;
}
