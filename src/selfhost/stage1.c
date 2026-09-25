/* A small target-side stage for the bootstrap smoke test. */

static int parse_expression(const char *text, int *position);

static int parse_primary(const char *text, int *position)
{
    int value = 0;
    if (text[*position] == '(') {
        ++*position;
        value = parse_expression(text, position);
        if (text[*position] == ')') ++*position;
        return value;
    }
    while (text[*position] >= '0' && text[*position] <= '9') {
        value = value * 10 + text[*position] - '0';
        ++*position;
    }
    return value;
}

static int parse_term(const char *text, int *position)
{
    int value = parse_primary(text, position);
    while (text[*position] == '*') {
        ++*position;
        value *= parse_primary(text, position);
    }
    return value;
}

static int parse_expression(const char *text, int *position)
{
    int value = parse_term(text, position);
    while (text[*position] == '+') {
        ++*position;
        value += parse_term(text, position);
    }
    return value;
}

int main(void)
{
    char source[] = "7*6+(4+5)";
    int position = 0;
    return parse_expression(source, &position) & 255;
}
