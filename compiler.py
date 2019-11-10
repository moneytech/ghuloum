#!/usr/bin/env python3.6

# http://scheme2006.cs.uchicago.edu/11-ghuloum.pdf


def emit(stream, text):
    stream.write(f"{text}\n")


FIXNUM_SHIFT = 2
FIXNUM_MASK = 0x3
CHAR_SHIFT = 8
CHAR_TAG = 0b00001111
BOOL_SHIFT = 7
BOOL_TAG = 0b0011111
BOOL_MASK = 0b1111111
NIL_TAG = 0b00101111
PAIR_TAG = 0b1
WORD_SIZE = 4
HEAP_PTR = "rsi"


class Var:
    def __init__(self, name):
        self.name = name


def is_immediate(x):
    return isinstance(x, (bool, int, str)) or x == []


def imm(x):
    if not is_immediate(x):
        raise ValueError(x)
    if isinstance(x, bool):
        return (x << BOOL_SHIFT) | BOOL_TAG
    if isinstance(x, int):
        return x << FIXNUM_SHIFT
    if isinstance(x, str):
        c = ord(x)
        return (c << CHAR_SHIFT) | CHAR_TAG
    if isinstance(x, list):
        assert x == []
        return NIL_TAG


def is_primcall(x):
    return isinstance(x, list) and len(x) >= 2 and x[0] in PRIMITIVE_TABLE


def prim_add1(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"add rax, {imm(1)}  ; increment")


def prim_sub1(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"sub rax, {imm(1)}  ; decrement")


def prim_int_to_char(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    # ints are already "shifted" 2 over
    emit(stream, f"shl rax, 6")
    emit(stream, f"add rax, {CHAR_TAG}  ; int->char")


def prim_char_to_int(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    # ints should have 2 trailing zeroes
    emit(stream, f"shr rax, 6  ; char->int")


def prim_zerop(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"cmp rax, 0  ; zero?")
    emit(stream, f"mov rax, 0")
    emit(stream, f"sete al")
    emit(stream, f"shl rax, {BOOL_SHIFT}")
    emit(stream, f"or rax, {BOOL_TAG}")


def prim_nullp(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"cmp rax, {NIL_TAG}  ; null?")
    emit(stream, f"mov rax, 0")
    emit(stream, f"sete al")
    emit(stream, f"shl rax, {BOOL_SHIFT}")
    emit(stream, f"or rax, {BOOL_TAG}")


def prim_not(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"xor rax, {BOOL_TAG}  ; not")
    emit(stream, f"mov rax, 0")
    emit(stream, f"sete al")
    emit(stream, f"shl rax, {BOOL_SHIFT}")
    emit(stream, f"or rax, {BOOL_TAG}")


def prim_integerp(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"and rax, {FIXNUM_MASK}  ; integer?")
    emit(stream, f"cmp rax, 0")
    emit(stream, f"mov rax, 0")
    emit(stream, f"sete al")
    emit(stream, f"shl rax, {BOOL_SHIFT}")
    emit(stream, f"or rax, {BOOL_TAG}")


def prim_booleanp(stream, arg, si, env):
    compile_expr(stream, arg, si, env)
    emit(stream, f"and rax, {BOOL_MASK}  ; boolean?")
    emit(stream, f"cmp rax, {BOOL_TAG}")
    emit(stream, f"mov rax, 0")
    emit(stream, f"sete al")
    emit(stream, f"shl rax, {BOOL_SHIFT}")
    emit(stream, f"or rax, {BOOL_TAG}")


def prim_binplus(stream, left, right, si, env):
    compile_expr(stream, right, si, env)
    emit(stream, f"mov [rsp-{si}], rax")
    compile_expr(stream, left, si + WORD_SIZE, env)
    emit(stream, f"add rax, [rsp-{si}]")


def prim_binminus(stream, left, right, si, env):
    compile_expr(stream, right, si, env)
    emit(stream, f"mov [rsp-{si}], rax")
    compile_expr(stream, left, si + WORD_SIZE, env)
    emit(stream, f"sub rax, [rsp-{si}]")


def prim_cons(stream, car, cdr, si, env):
    compile_expr(stream, car, si, env)
    emit(stream, f"mov [{HEAP_PTR}], rax  ; cons")
    compile_expr(stream, cdr, si, env)
    emit(stream, f"mov [{HEAP_PTR}+4], rax")
    emit(stream, f"mov rax, {HEAP_PTR}")
    emit(stream, f"or rax, {PAIR_TAG}")
    emit(stream, f"add {HEAP_PTR}, {WORD_SIZE}")


def prim_car(stream, expr, si, env):
    compile_expr(stream, expr, si, env)
    emit(stream, f"mov rax, [rax-1]  ; car")


def prim_cdr(stream, expr, si, env):
    compile_expr(stream, expr, si, env)
    emit(stream, f"mov rax, [rax+3]  ; cdr")


PRIMITIVE_TABLE = {
    "add1": prim_add1,
    "sub1": prim_sub1,
    "integer->char": prim_int_to_char,
    "char->integer": prim_char_to_int,
    "zero?": prim_zerop,
    "null?": prim_nullp,
    "not": prim_not,
    "integer?": prim_integerp,
    "boolean?": prim_booleanp,
    "+": prim_binplus,
    "-": prim_binminus,
    "cons": prim_cons,
    "car": prim_car,
    "cdr": prim_cdr,
}


def emit_primcall(stream, x, si, env):
    op, *args = x
    fn = PRIMITIVE_TABLE[op]
    fn(stream, *args, si, env)


def is_let(x):
    return isinstance(x, list) and len(x) == 3 and x[0] == "let"


def compile_let(stream, bindings, body, si, env):
    if not bindings:
        compile_expr(stream, body, si, env)
        return
    new_env = env.copy()
    name, expr = bindings[0]
    compile_expr(stream, expr, si, env)
    emit(stream, f"mov [rsp-{si}], rax")
    new_env[name] = si
    compile_let(stream, bindings[1:], body, si + WORD_SIZE, new_env)


LABEL_COUNTER = -1


def unique_label():
    global LABEL_COUNTER
    LABEL_COUNTER += 1
    return f"L{LABEL_COUNTER}"


def emit_label(stream, label):
    emit(stream, f"{label}:")


def is_if(x):
    return isinstance(x, list) and len(x) == 4 and x[0] == "if"


def compile_if(stream, cond, consequent, alternative, si, env):
    L0 = unique_label()
    L1 = unique_label()
    compile_expr(stream, cond, si, env)
    emit(stream, f"cmp rax, {imm(False)}")
    emit(stream, f"je {L0}")
    compile_expr(stream, consequent, si, env)
    emit(stream, f"jmp {L1}")
    emit_label(stream, L0)
    compile_expr(stream, alternative, si, env)
    emit_label(stream, L1)


def compile_expr(stream, x, si, env):
    if is_immediate(x):
        emit(stream, f"mov rax, {imm(x)}")
        return
    elif is_primcall(x):
        emit_primcall(stream, x, si, env)
        return
    elif isinstance(x, Var):
        # compile_var(stream, x)
        offset = env[x.name]
        emit(stream, f"mov rax, [rsp-{offset}]")
        return
    elif is_let(x):
        bindings = x[1]
        body = x[2]
        compile_let(stream, bindings, body, si, env)
        return
    elif is_if(x):
        compile_if(stream, *x[1:], si, env)
        return
    raise ValueError(x)


def compile_program(stream, x, env=None):
    emit(
        stream,
        """section .text
global scheme_entry
scheme_entry:
mov rsi, rdi""",
    )
    if env is None:
        env = {}
    compile_expr(stream, x, WORD_SIZE, env)
    emit(stream, "ret")


if __name__ == "__main__":
    with open("entry.s", "w") as f:
        compile_program(f, ["cdr", ["cons", 1, 2]])
