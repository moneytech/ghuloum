#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef _GNU_SOURCE

#include "libtap/tap.h"

// Objects
//
__attribute__((used)) static const int kFixnumTag = 0x0;
__attribute__((used)) static const int kPairTag = 0x1;
__attribute__((used)) static const int kVectorTag = 0x2;
__attribute__((used)) static const int kStringTag = 0x3;
__attribute__((used)) static const int kSymbolTag = 0x5;
__attribute__((used)) static const int kClosureTag = 0x6;
__attribute__((used)) static const int kCharTag = 0xf;
__attribute__((used)) static const int kBoolTag = 0x1f;
__attribute__((used)) static const int kNilTag = 0x2f;

__attribute__((used)) static const int kBoolMask = 0xf;
__attribute__((used)) static const int kBoolShift = 7;
__attribute__((used)) static const int kCharMask = 0xff;
__attribute__((used)) static const int kCharShift = 8;
__attribute__((used)) static const int kFixnumMask = 0x3;
__attribute__((used)) static const int kFixnumShift = 2;
__attribute__((used)) static const int kHeapObjectMask = 0x7;

int32_t encodeImmediateFixnum(int32_t f) {
  assert(f < 0x7fffffff && "too big");
  assert(f > -0x80000000L && "too small");
  return f << kFixnumShift;
}

int32_t encodeImmediateBool(bool value) {
  return ((value ? 1L : 0L) << kBoolShift) | kBoolTag;
}

int32_t encodeImmediateChar(char c) {
  return ((int32_t)c << kCharShift) | kCharTag;
}

// End Objects

// Machine code

typedef unsigned char byte;

typedef enum {
  kWritable,
  kExecutable,
} BufferState;

typedef struct {
  byte *address;
  size_t len;
  BufferState state;
} Buffer;

void Buffer_init(Buffer *result, size_t len) {
  result->address = mmap(/*addr=*/NULL, len, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE,
                         /*filedes=*/-1, /*off=*/0);
  assert(result->address != MAP_FAILED);
  result->len = len;
  result->state = kWritable;
}

void Buffer_deinit(Buffer *buf) {
  munmap(buf->address, buf->len);
  buf->address = NULL;
}

int Buffer_make_executable(Buffer *buf) {
  int result = mprotect(buf->address, buf->len, PROT_EXEC);
  buf->state = kExecutable;
  return result;
}

void Buffer_at_put(Buffer *buf, size_t pos, byte b) { buf->address[pos] = b; }

// TODO: dynamically expand buffer when space runs out
typedef struct {
  Buffer *buf;
  size_t pos;
} BufferWriter;

void BufferWriter_init(BufferWriter *writer, Buffer *buf) {
  writer->buf = buf;
  writer->pos = 0;
}

const int kBitsPerByte = 8; // bits
const int kWordSize = 8;    // bytes

void BufferWriter_backpatch_displacement_imm32(BufferWriter *writer,
                                               int32_t pos_after_jump) {
  int32_t relative = writer->pos - pos_after_jump;
  int32_t displacement_first_byte = pos_after_jump - sizeof(int32_t);
  for (size_t i = 0; i < sizeof(int32_t); i++) {
    Buffer_at_put(writer->buf, displacement_first_byte + i,
                  (relative >> (i * kBitsPerByte)) & 0xff);
  }
}
size_t BufferWriter_get_pos(BufferWriter *writer) { return writer->pos; }

void Buffer_dump(BufferWriter *writer, FILE *fp) {
  for (size_t i = 0; i < writer->pos; i++) {
    fprintf(fp, "%.2x ", writer->buf->address[i]);
  }
  fprintf(fp, "\n");
}

void Buffer_write8(BufferWriter *writer, byte b) {
  assert(writer->pos < writer->buf->len);
  Buffer_at_put(writer->buf, writer->pos++, b);
}

void Buffer_write_arr(BufferWriter *writer, byte *arr, size_t len) {
  for (size_t i = 0; i < len; i++) {
    Buffer_write8(writer, arr[i]);
  }
}

void Buffer_write32(BufferWriter *writer, int32_t value) {
  for (size_t i = 0; i < 4; i++) {
    Buffer_write8(writer, (value >> (i * kBitsPerByte)) & 0xff);
  }
}

typedef enum {
  kRax = 0,
  kRcx,
  kRdx,
  kRbx,
  kRsp,
  kRbp,
  kRsi,
  kRdi,
} Register;

typedef enum {
  kAl = 0,
} SubRegister;

typedef enum {
  kEqual,
} Condition;

void Buffer_inc_reg(BufferWriter *writer, Register reg) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0xff);
  Buffer_write8(writer, 0xc0 + reg);
}

void Buffer_dec_reg(BufferWriter *writer, Register reg) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0xff);
  Buffer_write8(writer, 0xc8 + reg);
}

void Buffer_mov_reg_imm32(BufferWriter *writer, Register dst, int32_t src) {
  Buffer_write8(writer, 0xb8 + dst);
  Buffer_write32(writer, src);
}

void Buffer_add_reg_imm32(BufferWriter *writer, Register dst, int32_t src) {
  if (dst == kRax) {
    // Optimization: add eax, {imm32} can either be encoded as 05 {imm32} or 81
    // c0 {imm32}.
    Buffer_write8(writer, 0x05);
  } else {
    Buffer_write8(writer, 0x81);
    Buffer_write8(writer, 0xc0 + dst);
  }
  Buffer_write32(writer, src);
}

void Buffer_add_reg_stack(BufferWriter *writer, Register dst, int8_t offset) {
  assert(offset < 0 && "positive stack offset unimplemented");
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x03);
  Buffer_write8(writer, 0x04 + (dst * 8) + (offset == 0 ? 0 : 0x40));
  Buffer_write8(writer, 0x24);
  Buffer_write8(writer, 0x100 + offset);
}

static uint8_t encode_disp(int8_t disp) {
  if (disp >= 0)
    return disp;
  return 0x100 + disp;
}

void Buffer_mov_rax_to_reg_disp(BufferWriter *writer, Register dst,
                                int8_t disp) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x89);
  Buffer_write8(writer, 0x40 + dst);
  Buffer_write8(writer, encode_disp(disp));
}

void Buffer_mov_reg_disp_to_rax(BufferWriter *writer, Register dst,
                                int8_t disp) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x8b);
  Buffer_write8(writer, 0x40 + dst);
  Buffer_write8(writer, encode_disp(disp));
}

void Buffer_sub_reg_imm32(BufferWriter *writer, Register dst, int32_t src) {
  if (dst == kRax) {
    // Optimization: sub eax, {imm32} can either be encoded as 2d {imm32} or 81
    // e8 {imm32}.
    Buffer_write8(writer, 0x2d);
  } else {
    Buffer_write8(writer, 0x83);
    Buffer_write8(writer, 0xe8 + dst);
  }
  Buffer_write32(writer, src);
}

void Buffer_mov_reg_reg(BufferWriter *writer, Register dst, Register src) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x89);
  Buffer_write8(writer, 0xc0 + dst + src * 8);
}

void Buffer_mov_reg_to_stack(BufferWriter *writer, Register src,
                             int8_t offset) {
  assert(offset < 0 && "positive stack offset unimplemented");
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x89);
  Buffer_write8(writer, 0x04 + (src * 8) + (offset == 0 ? 0 : 0x40));
  Buffer_write8(writer, 0x24);
  Buffer_write8(writer, 0x100 + offset);
}

void Buffer_mov_stack_to_reg(BufferWriter *writer, Register dst,
                             int8_t offset) {
  assert(offset < 0 && "positive stack offset unimplemented");
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x8b);
  Buffer_write8(writer, 0x04 + (dst * 8) + (offset == 0 ? 0 : 0x40));
  Buffer_write8(writer, 0x24);
  Buffer_write8(writer, 0x100 + offset);
}

void Buffer_shl_reg(BufferWriter *writer, Register dst, int8_t bits) {
  assert(bits >= 0 && "too few bits");
  assert(bits < 64 && "too many bits");
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0xc1);
  Buffer_write8(writer, 0xe0 + dst);
  Buffer_write8(writer, bits);
}

void Buffer_and_reg_imm32(BufferWriter *writer, Register dst, int32_t value) {
  Buffer_write8(writer, 0x48);
  if (dst == kRax) {
    // Optimization: and eax, {imm32} can either be encoded as 48 25 {imm32} or
    // 48 81 e0 {imm32}.
    Buffer_write8(writer, 0x25);
    Buffer_write32(writer, value);
    return;
  }
  Buffer_write8(writer, 0x81);
  Buffer_write8(writer, 0xe0 + dst);
  Buffer_write32(writer, value);
}

void Buffer_or_reg_imm32(BufferWriter *writer, Register dst, int32_t value) {
  Buffer_write8(writer, 0x48);
  if (dst == kRax) {
    // Optimization: or eax, {imm32} can either be encoded as 48 0d {imm32} or
    // 48 81 c8 {imm32}.
    Buffer_write8(writer, 0x0d);
    Buffer_write32(writer, value);
    return;
  }
  Buffer_write8(writer, 0x81);
  Buffer_write8(writer, 0xc8 + dst);
  Buffer_write32(writer, value);
}

void Buffer_cmp_reg_imm32(BufferWriter *writer, Register dst, int32_t value) {
  if (dst == kRax) {
    // Optimization: cmp eax, {imm32} can either be encoded as 48 3d {imm32} or
    // 48 81 f8 {imm32}.
    Buffer_write8(writer, 0x48);
    Buffer_write8(writer, 0x3d);
    Buffer_write32(writer, value);
    return;
  }
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x81);
  Buffer_write8(writer, 0xf8 + dst);
  Buffer_write32(writer, value);
}

void Buffer_setcc_reg(BufferWriter *writer, Condition cond, SubRegister dst) {
  assert(cond == kEqual && "other conditions unimplemented");
  Buffer_write8(writer, 0x0f);
  Buffer_write8(writer, 0x94);
  Buffer_write8(writer, 0xc0 + dst);
}

// Relative jump
void Buffer_je_imm32(BufferWriter *writer, int32_t disp) {
  assert(disp > 0 && "negative disp unimplemented");
  Buffer_write8(writer, 0x0f);
  Buffer_write8(writer, 0x84);
  Buffer_write32(writer, disp);
}

// Relative jump
void Buffer_jmp_imm32(BufferWriter *writer, int32_t disp) {
  assert(disp > 0 && "negative disp unimplemented");
  Buffer_write8(writer, 0xe9);
  Buffer_write32(writer, disp);
}

static uint32_t encode_disp32(int32_t disp) {
  if (disp >= 0)
    return disp;
  return 0x100000000 + disp;
}

// Relative call
void Buffer_call_imm32(BufferWriter *writer, int32_t disp) {
  disp -= 5; // sizeof call instruction (e8 + imm32)
  Buffer_write8(writer, 0xe8);
  Buffer_write32(writer, encode_disp32(disp));
}

void Buffer_ret(BufferWriter *writer) { Buffer_write8(writer, 0xc3); }

// End Machine code

// Env

// An Env maps a list of names to a list of corresponding indices in the stack.
// This is useful when binding names in function params, `let`, and at the top
// level.
//
// Goal: don't allocate any EnvNodes on the heap when recursively compiling
// functions and let-bindings; instead, borrow the atom char* and store an
// EnvNode on the stack. All recursive leaf calls will be able to reference
// this EnvNode and when the function returns, the name should not be
// available/bound anyway. Eg:
//
// void AST_compile_let(..., ASTNode *bindings, ASTNode *body) {
//   if (bindings == nil) {
//     AST_compile_expr(..., body);
//     return;
//   }
//   EnvNode new_env = bind_name(car(bindings));
//   new_env.next = ctx->locals;
//   CompilerContext new_ctx = CompilerContext_with_locals(..., &new_env);
//   AST_compile_let(&new_ctx, ..., cdr(bindings), body);
// }
//
// In any case, the atoms/char* will live long enough since we don't have any
// plan to free ASTNodes right now...

typedef struct EnvNode {
  char *name;
  int32_t stack_index;
  struct EnvNode *next;
} EnvNode;

EnvNode Env_init(char *name, int32_t stack_index, EnvNode *next) {
  return (EnvNode){.name = name, .stack_index = stack_index, .next = next};
}

// Return true if found and store the corresponding stack_index in
// *stack_index. Return false otherwise.
bool Env_lookup(EnvNode *env, char *name, int32_t *stack_index) {
  assert(name != NULL);
  assert(stack_index != NULL);
  if (env == NULL)
    return false;
  if (env->name == name || strcmp(env->name, name) == 0) {
    *stack_index = env->stack_index;
    return true;
  }
  return Env_lookup(env->next, name, stack_index);
}

// End Env

// AST

typedef enum {
  kFixnum,
  kAtom,
  kCons,
} ASTNodeType;

struct ASTNode;

typedef struct {
  struct ASTNode *car;
  struct ASTNode *cdr;
} ASTCons;

// TODO: tag AST nodes as values. immediate ints, etc
typedef struct ASTNode {
  ASTNodeType type;
  union {
    int fixnum;
    char *atom;
    ASTCons cons;
  } value;
} ASTNode;

ASTNode nil_struct = {.type = kCons, .value.cons = {.car = NULL, .cdr = NULL}};
ASTNode *nil = &nil_struct;

ASTNode *AST_new_fixnum(int fixnum) {
  ASTNode *result = malloc(sizeof *result);
  result->type = kFixnum;
  result->value.fixnum = fixnum;
  return result;
}

ASTNode *AST_new_atom(char *atom) {
  ASTNode *result = malloc(sizeof *result);
  result->type = kAtom;
  result->value.atom = strdup(atom);
  return result;
}

ASTNode *AST_new_cons(ASTNode *car, ASTNode *cdr) {
  if (car == NULL && cdr == NULL)
    return nil;
  assert(car != NULL && "both car & cdr must be NULL, or neither");
  assert(cdr != NULL && "both car & cdr must be NULL, or neither");
  ASTNode *result = malloc(sizeof *result);
  result->type = kCons;
  result->value.cons.car = car;
  result->value.cons.cdr = cdr;
  return result;
}

int AST_is_atom(ASTNode *node) { return node->type == kAtom; }

int AST_atom_equals_cstr(ASTNode *node, char *cstr) {
  assert(AST_is_atom(node));
  return strcmp(node->value.atom, cstr) == 0;
}

ASTNode *AST_car(ASTNode *cons) {
  assert(cons != nil);
  assert(cons->type == kCons);
  return cons->value.cons.car;
}

ASTNode *AST_cdr(ASTNode *cons) {
  assert(cons != nil);
  assert(cons->type == kCons);
  return cons->value.cons.cdr;
}

// Reader

void advance(int *pos) { ++*pos; }

ASTNode *Reader_read_number(char *input, int *pos) {
  char c = '\0';
  int value = 0;
  while (isdigit(c = input[*pos])) {
    value *= 10;
    value += c - '0';
    advance(pos);
  }
  return AST_new_fixnum(value);
}

const int ATOM_MAX = 32;

bool isatomchar(char c) { return isalpha(c) || c == '+' || c == '-'; }

ASTNode *Reader_read_atom(char *input, int *pos) {
  char buf[ATOM_MAX + 1]; // +1 for NUL
  int length = 0;
  while (length < ATOM_MAX && isatomchar(buf[length] = input[*pos])) {
    advance(pos);
    length++;
  }
  buf[length] = '\0';
  return AST_new_atom(buf);
}

ASTNode *Reader_read_rec(char *input, int *pos);

ASTNode *Reader_read_list(char *input, int *pos) {
  if (input[*pos] == ')') {
    advance(pos);
    return nil;
  }
  ASTNode *car = Reader_read_rec(input, pos);
  assert(car != NULL);
  ASTNode *cdr = Reader_read_list(input, pos);
  assert(cdr != NULL);
  return AST_new_cons(car, cdr);
}

ASTNode *Reader_read_rec(char *input, int *pos) {
  char c = '\0';
  while (isspace(c = input[*pos])) {
    advance(pos);
  }
  if (isdigit(c)) {
    return Reader_read_number(input, pos);
  }
  if (isatomchar(c)) {
    return Reader_read_atom(input, pos);
  }
  if (c == '(') {
    advance(pos); // skip '('
    return Reader_read_list(input, pos);
  }
  return NULL;
}

ASTNode *Reader_read(char *input) {
  int pos = 0;
  return Reader_read_rec(input, &pos);
}

// End Reader

// Compiler context

// Does not include stack index because that is modified a lot when recursing
// I may end up being annoyed about this for Env, too
typedef struct {
  BufferWriter *writer;
  EnvNode *labels;
  EnvNode *locals;
  // TODO: add formals separately from locals?
} CompilerContext;

void CompilerContext_init(CompilerContext *ctx, BufferWriter *writer,
                          EnvNode *labels, EnvNode *locals) {
  assert(ctx != NULL);
  ctx->writer = writer;
  ctx->labels = labels;
  ctx->locals = locals;
}

CompilerContext CompilerContext_with_labels(CompilerContext *ctx,
                                            EnvNode *labels) {
  CompilerContext result = *ctx;
  result.labels = labels;
  return result;
}

CompilerContext CompilerContext_with_locals(CompilerContext *ctx,
                                            EnvNode *locals) {
  CompilerContext result = *ctx;
  result.locals = locals;
  return result;
}

// End Compiler context

// env is a map of variables to stack locations
int AST_compile_expr(CompilerContext *ctx, ASTNode *node, int stack_index);

ASTNode *operand1(ASTNode *args) { return AST_car(args); }
ASTNode *operand2(ASTNode *args) { return AST_car(AST_cdr(args)); }
ASTNode *operand3(ASTNode *args) { return AST_car(AST_cdr(AST_cdr(args))); }

int AST_compile_let(CompilerContext *ctx, ASTNode *bindings, ASTNode *body,
                    int stack_index) {
  if (bindings == nil) {
    // Base case: no bindings. Emit the body.
    AST_compile_expr(ctx, body, stack_index);
    return 0;
  }
  // Inductive case: some bindings. Emit code for the first binding, bind the
  // name to the stack index, and recurse.
  ASTNode *first_binding = AST_car(bindings);
  ASTNode *name = AST_car(first_binding);
  assert(name && name->type == kAtom && "name must be an atom");
  ASTNode *expr = AST_car(AST_cdr(first_binding));
  AST_compile_expr(ctx, expr, stack_index);
  Buffer_mov_reg_to_stack(ctx->writer, kRax, stack_index);
  EnvNode new_locals = Env_init(name->value.atom, stack_index, ctx->locals);
  CompilerContext new_ctx = CompilerContext_with_locals(ctx, &new_locals);
  return AST_compile_let(&new_ctx, AST_cdr(bindings), body,
                         stack_index - kWordSize);
}

// http://ref.x86asm.net/coder32.html
// https://www.felixcloutier.com/x86/index.html
// rasm2 -D -b64 "48 89 44 24 f8 "
//  -> or -d

int AST_compile_if(CompilerContext *ctx, ASTNode *test, ASTNode *iftrue,
                   ASTNode *iffalse, int stack_index) {
  AST_compile_expr(ctx, test, stack_index);
  Buffer_cmp_reg_imm32(ctx->writer, kRax, encodeImmediateBool(false));
  Buffer_je_imm32(ctx->writer, 0x12345678);
  int iffalse_pos = BufferWriter_get_pos(ctx->writer);
  AST_compile_expr(ctx, iftrue, stack_index);
  Buffer_jmp_imm32(ctx->writer, 0x1a2b3c4d);
  int end_pos = BufferWriter_get_pos(ctx->writer);
  BufferWriter_backpatch_displacement_imm32(ctx->writer, iffalse_pos);
  AST_compile_expr(ctx, iffalse, stack_index);
  BufferWriter_backpatch_displacement_imm32(ctx->writer, end_pos);
  return 0;
}

int AST_compile_cons(CompilerContext *ctx, ASTNode *car, ASTNode *cdr,
                     int stack_index) {
  AST_compile_expr(ctx, car, stack_index - kWordSize);
  // Set car
  Buffer_mov_rax_to_reg_disp(ctx->writer, kRsi, 0);
  AST_compile_expr(ctx, cdr, stack_index);
  // Set cdr
  Buffer_mov_rax_to_reg_disp(ctx->writer, kRsi, kWordSize);
  // Tag the pointer
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRax, /*src=*/kRsi);
  Buffer_or_reg_imm32(ctx->writer, /*dst=*/kRax, 1);
  // Bump heap
  Buffer_add_reg_imm32(ctx->writer, /*dst=*/kRsi, 2 * kWordSize);
  return 0;
}

int AST_compile_code(CompilerContext *ctx, ASTNode *formals, ASTNode *body,
                     int stack_index) {
  if (formals == nil) {
    int result = AST_compile_expr(ctx, body, stack_index);
    if (result != 0) {
      return result;
    }
    Buffer_ret(ctx->writer);
    return 0;
  }
  ASTNode *name = AST_car(formals);
  EnvNode new_locals = Env_init(name->value.atom, stack_index, ctx->locals);
  CompilerContext new_ctx = CompilerContext_with_locals(ctx, &new_locals);
  return AST_compile_code(&new_ctx, AST_cdr(formals), body,
                          stack_index - kWordSize);
}

int AST_compile_labelcall(CompilerContext *ctx, int32_t code_pos, ASTNode *args,
                          int stack_index) {
  assert(args->type == kCons);
  if (args == nil) {
    int32_t disp = code_pos - BufferWriter_get_pos(ctx->writer);
    Buffer_call_imm32(ctx->writer, disp);
    return 0;
  }
  ASTNode *arg = AST_car(args);
  int result = AST_compile_expr(ctx, arg, stack_index);
  if (result != 0) {
    return result;
  }
  Buffer_mov_reg_to_stack(ctx->writer, kRax, stack_index);
  return AST_compile_labelcall(ctx, code_pos, AST_cdr(args),
                               stack_index - kWordSize);
}

int AST_compile_call(CompilerContext *ctx, ASTNode *fnexpr, ASTNode *args,
                     int stack_index) {
  if (AST_is_atom(fnexpr)) {
    // Assumed to be a primcall
    if (AST_atom_equals_cstr(fnexpr, "add1")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      Buffer_add_reg_imm32(ctx->writer, kRax, encodeImmediateFixnum(1));
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "sub1")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      Buffer_sub_reg_imm32(ctx->writer, kRax, encodeImmediateFixnum(1));
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "integer->char")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      Buffer_shl_reg(ctx->writer, kRax, /*bits=*/kCharShift - kFixnumShift);
      // TODO: generate more compact code since we know we're only or-ing with a
      // byte
      Buffer_or_reg_imm32(ctx->writer, kRax, kCharTag);
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "zero?")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      Buffer_cmp_reg_imm32(ctx->writer, kRax, 0);
      Buffer_mov_reg_imm32(ctx->writer, kRax, 0);
      Buffer_setcc_reg(ctx->writer, kEqual, kAl);
      Buffer_shl_reg(ctx->writer, kRax, kBoolShift);
      Buffer_or_reg_imm32(ctx->writer, kRax, kBoolTag);
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "+")) {
      AST_compile_expr(ctx, operand2(args), stack_index);
      Buffer_mov_reg_to_stack(ctx->writer, kRax, /*offset=*/stack_index);
      AST_compile_expr(ctx, operand1(args), stack_index - kWordSize);
      Buffer_add_reg_stack(ctx->writer, kRax, /*offset=*/stack_index);
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "let")) {
      return AST_compile_let(ctx, /*bindings=*/operand1(args),
                             /*body=*/operand2(args), stack_index);
    }
    if (AST_atom_equals_cstr(fnexpr, "if")) {
      // TODO: in if, rewrite empty iffalse => '()
      return AST_compile_if(ctx, operand1(args), operand2(args), operand3(args),
                            stack_index);
    }
    if (AST_atom_equals_cstr(fnexpr, "cons")) {
      return AST_compile_cons(ctx, operand1(args), operand2(args), stack_index);
    }
    if (AST_atom_equals_cstr(fnexpr, "car")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      // Since heap addresses are biased by 1, the car of a cons cell is at
      // offset -1, instead of 0.
      Buffer_mov_reg_disp_to_rax(ctx->writer, /*src=*/kRax, -1);
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "cdr")) {
      AST_compile_expr(ctx, operand1(args), stack_index);
      // Since heap addresses are biased by 1, the cdr of a cons cell is at
      // offset 7, instead of 8.
      Buffer_mov_reg_disp_to_rax(ctx->writer, /*src=*/kRax, kWordSize - 1);
      return 0;
    }
    if (AST_atom_equals_cstr(fnexpr, "code")) {
      // The flow of control enters `code` in a new call frame. The stack looks
      // like this:
      //
      // low addr
      // --------
      // .
      // .
      // .
      // rsp   24: arg3
      // rsp - 16: arg2
      // rsp - 8 : arg1
      // rsp     : return addr
      // ~~~~~~~~~~~
      // .
      // .
      // .
      // ---------
      // high addr
      //
      // Start stack_index over at -kWordSize -- the location of the first
      // formal -- since the return address is at rsp.
      return AST_compile_code(ctx, /*formals=*/operand1(args),
                              /*body=*/operand2(args), -kWordSize);
    }
    if (AST_atom_equals_cstr(fnexpr, "labelcall")) {
      ASTNode *label = operand1(args);
      assert(AST_is_atom(label));
      char *name = label->value.atom;
      int32_t code_pos;
      if (!Env_lookup(ctx->labels, name, &code_pos)) {
        fprintf(stderr, "Unbound label: `%s'\n", name);
        return -1;
      }
      return AST_compile_labelcall(ctx, /*code_pos=*/code_pos,
                                   /*args=*/AST_cdr(args), stack_index);
    }
    assert(0 && "unknown call");
  }
  assert(0 && "unknown call");
}

int AST_compile_expr(CompilerContext *ctx, ASTNode *node, int stack_index) {
  switch (node->type) {
  case kFixnum: {
    uint32_t value = (uint32_t)node->value.fixnum;
    Buffer_mov_reg_imm32(ctx->writer, kRax, encodeImmediateFixnum(value));
    return 0;
  }
  case kCons: {
    // Assumed to be in the form (<expr> <op1> <op2> ...)
    return AST_compile_call(ctx, AST_car(node), AST_cdr(node), stack_index);
  }
  case kAtom: {
    // TODO: confusing that it shadows the parameter. fix
    int32_t stack_index;
    char *name = node->value.atom;
    if (!Env_lookup(ctx->locals, name, &stack_index)) {
      fprintf(stderr, "Unbound variable: `%s'\n", name);
      return -1;
    }
    Buffer_mov_stack_to_reg(ctx->writer, kRax, stack_index);
    return 0;
  }
  }
  assert(false && "unhandled expression type");
  return -1;
}

// TODO: naming confusing because we have no concept of functions, really
int AST_compile_function(CompilerContext *ctx, ASTNode *node) {
  int result = AST_compile_expr(ctx, node, -kWordSize);
  if (result != 0) {
    return result;
  }
  Buffer_ret(ctx->writer);
  return 0;
}

int AST_compile_entry(CompilerContext *ctx, ASTNode *node) {
  // Save the heap in rsi, our global heap pointer
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRsi, /*src=*/kRdi);
  return AST_compile_function(ctx, node);
}

// labels is an environment mapping labels to code locations
// this is different from other environments that track stack locations of
// local variables and parameters
// (labels ((lvar (code (...) ...)) ...)
//         <exp>)
// Example -2: No labels
// 	(labels ()
// 	  5)
// Example -1: One label and don't use it
// 	(labels (
// 		(const (code () 5))
// 	 	)
// 	  5)
// Example 0: No parameters
// 	(labels (
// 		(const (code () 5))
// 	 	)
// 	  (labelcall const))
// Example 1: One param; return it
// 	(labels (
// 		(id (code (x) x))
// 	 	)
// 	  (labelcall id 5))
// Example 2: Two params and use them
// 	(labels (
// 		(add (code (x y) (+ x y)))
// 	 	)
// 	  (labelcall add 1 2))
// Example 3: Multiple labels in let* form (labels can use labels before them)
// 	(labels (
// 		(id (code (x) x))
// 		(add (code (x y) (+ (labelcall id x) y)))
// 	 	)
// 	  (labelcall add 1 2))
int AST_compile_labels(CompilerContext *ctx, ASTNode *bindings, ASTNode *body,
                       int body_pos, int stack_index) {
  if (bindings == nil) {
    // Emit body and backpatch jump to it
    BufferWriter_backpatch_displacement_imm32(ctx->writer, body_pos);
    return AST_compile_entry(ctx, body);
  }
  ASTNode *binding = AST_car(bindings);
  ASTNode *name = AST_car(binding);
  assert(name->type == kAtom);
  ASTNode *exp = AST_car(AST_cdr(binding));
  EnvNode new_labels = Env_init(name->value.atom,
                                BufferWriter_get_pos(ctx->writer), ctx->labels);
  CompilerContext new_ctx = CompilerContext_with_labels(ctx, &new_labels);
  int result = AST_compile_expr(&new_ctx, exp, stack_index);
  if (result != 0) {
    return result;
  }
  return AST_compile_labels(&new_ctx, /*bindings=*/AST_cdr(bindings), body,
                            body_pos, stack_index);
}

ASTNode *AST_tag(ASTNode *node) {
  assert(node->type == kCons);
  ASTNode *tag = AST_car(node);
  assert(tag->type == kAtom);
  return tag;
}

// (labels ((lvar <lexp>) ...)
//         <exp>)
int AST_compile_prog(CompilerContext *ctx, ASTNode *prog) {
  assert(prog->type == kCons);
  ASTNode *tag = AST_tag(prog);
  assert(tag->type == kAtom);
  assert(AST_atom_equals_cstr(tag, "labels"));
  ASTNode *args = AST_cdr(prog);
  // Jump to body
  Buffer_jmp_imm32(ctx->writer, 0x12345678);
  int body_pos = BufferWriter_get_pos(ctx->writer);
  // Emit labels & label-expressions
  ASTNode *body = operand2(args);
  return AST_compile_labels(ctx, /*bindings=*/operand1(args), body, body_pos,
                            /*stack_index=*/-kWordSize);
}

// End AST

// Testing

typedef uint64_t (*EntryFunction)(uint64_t);

uint64_t Testing_call_entry(Buffer *buf, uint64_t heap) {
  assert(buf != NULL);
  assert(buf->address != NULL);
  assert(buf->state == kExecutable);
  // The pointer-pointer cast is allowed but the underlying
  // data-to-function-pointer back-and-forth is only guaranteed to work on
  // POSIX systems (because of eg dlsym).
  EntryFunction function = *(EntryFunction *)(&buf->address);
  return function(heap);
}

void run_test(void (*test_body)(CompilerContext *, uint64_t)) {
  Buffer buf;
  Buffer_init(&buf, 100);
  void *heap = malloc(100 * kWordSize);
  {
    BufferWriter writer;
    BufferWriter_init(&writer, &buf);
    CompilerContext ctx;
    CompilerContext_init(&ctx, /*writer=*/&writer, /*labels=*/NULL,
                         /*locals=*/NULL);
    test_body(&ctx, (uint64_t)heap);
  }
  free(heap);
  Buffer_deinit(&buf);
}

#define EXPECT_EQUALS_BYTES(buf, arr)                                          \
  {                                                                            \
    int result =                                                               \
        cmp_ok(memcmp(buf->address, arr, sizeof arr), "==", 0, __func__);      \
    if (!result) {                                                             \
      printf("NOT EQUAL. Expected: ");                                         \
      for (size_t i = 0; i < sizeof arr; i++) {                                \
        printf("%.2x ", arr[i]);                                               \
      }                                                                        \
      printf("\n           Found:    ");                                       \
      for (size_t i = 0; i < ctx->writer->pos; i++) {                          \
        printf("%.2x ", buf->address[i]);                                      \
      }                                                                        \
      printf("\n");                                                            \
    }                                                                          \
  }

#define EXPECT_CALL_EQUALS(buf, expected)                                      \
  cmp_ok(Testing_call_entry(buf, heap), "==", expected, __func__)

#define TEST(name)                                                             \
  static void test_##name(CompilerContext *ctx,                                \
                          __attribute__((unused)) uint64_t heap)

uint64_t Run_from_cstr(char *input, CompilerContext *ctx, uint64_t heap) {
  ASTNode *node = Reader_read(input);
  int compile_result = AST_compile_entry(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  Buffer_make_executable(ctx->writer->buf);
  return Testing_call_entry(ctx->writer->buf, heap);
}

TEST(write_bytes_manually) {
  byte arr[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
  Buffer_write_arr(ctx->writer, arr, sizeof arr);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, 42);
}

TEST(write_bytes_manually2) {
  byte arr[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xff, 0xc0, 0xc3};
  Buffer_write_arr(ctx->writer, arr, sizeof arr);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, 43);
}

TEST(mov_rax_imm32) {
  Buffer_mov_reg_imm32(ctx->writer, kRax, 42);
  byte expected[] = {0xb8, 0x2a, 0x00, 0x00, 0x00};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(mov_rcx_imm32) {
  Buffer_mov_reg_imm32(ctx->writer, kRcx, 42);
  byte expected[] = {0xb9, 0x2a, 0x00, 0x00, 0x00};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(mov_inc) {
  Buffer_mov_reg_imm32(ctx->writer, kRax, 42);
  Buffer_inc_reg(ctx->writer, kRax);
  Buffer_ret(ctx->writer);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, 43);
}

TEST(mov_rax_rax) {
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRax, /*src=*/kRax);
  byte expected[] = {0x48, 0x89, 0xc0};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(mov_rax_rsi) {
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRax, /*src=*/kRsi);
  byte expected[] = {0x48, 0x89, 0xf0};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(mov_rdi_rbp) {
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRdi, /*src=*/kRbp);
  byte expected[] = {0x48, 0x89, 0xef};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(compile_fixnum) {
  // 123
  ASTNode *node = AST_new_fixnum(123);
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, 123; ret
  byte expected[] = {0xb8, 0xec, 0x01, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(123));
  free(node);
}

ASTNode *list1(ASTNode *e0) { return AST_new_cons(e0, nil); }

ASTNode *list2(ASTNode *e0, ASTNode *e1) { return AST_new_cons(e0, list1(e1)); }

ASTNode *list3(ASTNode *e0, ASTNode *e1, ASTNode *e2) {
  return AST_new_cons(e0, list2(e1, e2));
}

ASTNode *list4(ASTNode *e0, ASTNode *e1, ASTNode *e2, ASTNode *e3) {
  return AST_new_cons(e0, list3(e1, e2, e3));
}

TEST(compile_primcall_add1) {
  // (add1 5)
  ASTNode *node = list2(AST_new_atom("add1"), AST_new_fixnum(5));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); add eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x05,
                     0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(6));
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_sub1) {
  // (sub1 5)
  ASTNode *node = list2(AST_new_atom("sub1"), AST_new_fixnum(5));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); sub eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x2d,
                     0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(4));
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_add1_sub1) {
  // (sub1 (add1 5))
  ASTNode *node = list2(AST_new_atom("sub1"),
                        list2(AST_new_atom("add1"), AST_new_fixnum(5)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); add eax, imm(1); sub eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00,
                     0x00, 0x00, 0x2d, 0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_sub1_add1) {
  // (add1 (sub1 5))
  ASTNode *node = list2(AST_new_atom("add1"),
                        list2(AST_new_atom("sub1"), AST_new_fixnum(5)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); sub eax, imm(1); add eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x2d, 0x04, 0x00,
                     0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
  // TODO: figure out how to collect ASTs
}

TEST(compile_add_two_ints) {
  // (+ 1 2)
  ASTNode *node =
      list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_fixnum(2));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(2); mov [rsp-8], rax; mov rax, imm(1); add rax, [rsp-8]
  byte expected[] = {0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89,
                     0x44, 0x24, 0xf8, 0xb8, 0x04, 0x00, 0x00,
                     0x00, 0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(3));
  // TODO: figure out how to collect ASTs
}

TEST(compile_add_three_ints) {
  // (+ 1 (+ 2 3))
  ASTNode *node =
      list3(AST_new_atom("+"), AST_new_fixnum(1),
            list3(AST_new_atom("+"), AST_new_fixnum(2), AST_new_fixnum(3)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 0c 00 00 00          mov    eax,0xc
  // 5:  48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // a:  b8 08 00 00 00          mov    eax,0x8
  // f:  48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 14: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 19: b8 04 00 00 00          mov    eax,0x4
  // 1e: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 23: c3                      ret
  byte expected[] = {0xb8, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
                     0xf8, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x03, 0x44,
                     0x24, 0xf8, 0x48, 0x89, 0x44, 0x24, 0xf8, 0xb8, 0x04,
                     0x00, 0x00, 0x00, 0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(6));
  // TODO: figure out how to collect ASTs
}

TEST(compile_add_four_ints) {
  // (+ (+ 1 2) (+ 3 4))
  ASTNode *node =
      list3(AST_new_atom("+"),
            list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_fixnum(2)),
            list3(AST_new_atom("+"), AST_new_fixnum(3), AST_new_fixnum(4)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 10 00 00 00          mov    eax,0x10
  // 5:  48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // a:  b8 0c 00 00 00          mov    eax,0xc
  // f:  48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 14: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 19: b8 08 00 00 00          mov    eax,0x8
  // 1e: 48 89 44 24 f0          mov    QWORD PTR [rsp-0x10],rax
  // 23: b8 04 00 00 00          mov    eax,0x4
  // 28: 48 03 44 24 f0          add    rax,QWORD PTR [rsp-0x10]
  // 2d: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 32: c3                      ret
  byte expected[] = {0xb8, 0x10, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
                     0xf8, 0xb8, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x03, 0x44,
                     0x24, 0xf8, 0x48, 0x89, 0x44, 0x24, 0xf8, 0xb8, 0x08,
                     0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0xf0, 0xb8,
                     0x04, 0x00, 0x00, 0x00, 0x48, 0x03, 0x44, 0x24, 0xf0,
                     0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(10));
  // TODO: figure out how to collect ASTs
}

TEST(integer_to_char) {
  // (integer->char 65)
  ASTNode *node = list2(AST_new_atom("integer->char"), AST_new_fixnum(65));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 04 01 00 00          mov    eax,0x104
  // 5:  48 c1 e0 06             shl    rax,0x6
  // 9:  48 25 0f 00 00 00       and    rax,0xf
  // f:  c3                      ret
  byte expected[] = {0xb8, 0x04, 0x01, 0x00, 0x00, 0x48, 0xc1, 0xe0,
                     0x06, 0x48, 0x0d, 0x0f, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateChar('A'));
  // TODO: figure out how to collect ASTs
}

ASTNode *call1(char *fnname, ASTNode *arg) {
  return list2(AST_new_atom(fnname), arg);
}

TEST(zerop_with_zero_returns_true) {
  // (zero? (sub1 (add1 0)))
  ASTNode *node =
      call1("zero?", call1("sub1", call1("add1", AST_new_fixnum(0))));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // -> prelude
  // 0:  b8 00 00 00 00          mov    eax,0x0
  // 5:  05 04 00 00 00          add    eax,0x4
  // a:  2d 04 00 00 00          sub    eax,0x4
  // -> body of zero?
  // f:  48 3d 00 00 00 00       cmp    rax,0x0
  // 15: b8 00 00 00 00          mov    eax,0x0
  // 1a: 0f 94 c0                sete   al
  // 1d: 48 c1 e0 07             shl    rax,0x7
  // 21: 48 0d 1f 00 00 00       or     rax,0x1f
  // 27: c3                      ret
  byte expected[] = {0xb8, 0x00, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00,
                     0x00, 0x00, 0x2d, 0x04, 0x00, 0x00, 0x00, 0x48,
                     0x3d, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00,
                     0x00, 0x00, 0x0f, 0x94, 0xc0, 0x48, 0xc1, 0xe0,
                     0x07, 0x48, 0x0d, 0x1f, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateBool(true));
  // TODO: figure out how to collect ASTs
}

TEST(zerop_with_non_zero_returns_false) {
  // (zero? (sub1 (add1 0)))
  ASTNode *node =
      call1("zero?", call1("sub1", call1("add1", AST_new_fixnum(1))));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // -> prelude
  // 0:  b8 00 00 00 00          mov    eax,0x0
  // 5:  05 04 00 00 00          add    eax,0x4
  // a:  2d 04 00 00 00          sub    eax,0x4
  // -> body of zero?
  // f:  48 3d 00 00 00 00       cmp    rax,0x0
  // 15: b8 00 00 00 00          mov    eax,0x0
  // 1a: 0f 94 c0                sete   al
  // 1d: 48 c1 e0 07             shl    rax,0x7
  // 21: 48 0d 1f 00 00 00       or     rax,0x1f
  // 27: c3                      ret
  byte expected[] = {0xb8, 0x04, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00,
                     0x00, 0x00, 0x2d, 0x04, 0x00, 0x00, 0x00, 0x48,
                     0x3d, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00,
                     0x00, 0x00, 0x0f, 0x94, 0xc0, 0x48, 0xc1, 0xe0,
                     0x07, 0x48, 0x0d, 0x1f, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateBool(false));
  // TODO: figure out how to collect ASTs
}

TEST(let_with_no_bindings) {
  // (let () (+ 1 2))
  ASTNode *node = list3(
      AST_new_atom("let"),
      /*bindings*/ nil,
      /*body*/ list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_fixnum(2)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(2); mov [rsp-8], rax; mov rax, imm(1); add rax, [rsp-8]
  byte expected[] = {0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89,
                     0x44, 0x24, 0xf8, 0xb8, 0x04, 0x00, 0x00,
                     0x00, 0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(3));
  // TODO: figure out how to collect ASTs
}

TEST(let_with_one_binding) {
  // (let ((x 2)) (+ 1 x))
  ASTNode *node = list3(
      AST_new_atom("let"),
      /*bindings*/ list1(list2(AST_new_atom("x"), AST_new_fixnum(2))),
      /*body*/ list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_atom("x")));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 08 00 00 00          mov    eax,0x08
  // 5:  48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // a:  48 8b 44 24 f8          mov    rax,QWORD PTR [rsp-0x8]
  // f:  48 89 44 24 f0          mov    QWORD PTR [rsp-0x10],rax
  // 14: b8 04 00 00 00          mov    eax,0x4
  // 19: 48 03 44 24 f0          add    rax,QWORD PTR [rsp-0x10]
  // 1e: c3                      ret
  byte expected[] = {0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44,
                     0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x48,
                     0x89, 0x44, 0x24, 0xf0, 0xb8, 0x04, 0x00, 0x00,
                     0x00, 0x48, 0x03, 0x44, 0x24, 0xf0, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(3));
  // TODO: figure out how to collect ASTs
}

TEST(compile_atom_with_undefined_variable) {
  ASTNode *node = AST_new_atom("foo");
  int result = AST_compile_expr(ctx, node, /*stack_index=*/0);
  cmp_ok(result, "==", -1, __func__);
  // TODO: figure out how to collect ASTs
}

TEST(compile_atom_in_env_emits_stack_index) {
  ASTNode *node = AST_new_atom("foo");
  EnvNode locals = Env_init("foo", -34, /*next=*/NULL);
  CompilerContext new_ctx = CompilerContext_with_locals(ctx, &locals);
  int result = AST_compile_expr(&new_ctx, node, /*stack_index=*/0);
  cmp_ok(result, "==", 0, __func__);
  byte expected[] = {0x48, 0x8b, 0x44, 0x24, 0x100 - 34};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  // TODO: figure out how to collect ASTs
}

TEST(compile_if_test_true) {
  // (if (zero? 0) (+ 1 2) (+ 3 4))
  ASTNode *node =
      list4(AST_new_atom("if"), list2(AST_new_atom("zero?"), AST_new_fixnum(0)),
            list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_fixnum(2)),
            list3(AST_new_atom("+"), AST_new_fixnum(3), AST_new_fixnum(4)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 00 00 00 00          mov    eax,0x0
  // -> zero?
  // 5:  48 3d 00 00 00 00       cmp    rax,0x0
  // b:  b8 00 00 00 00          mov    eax,0x0
  // 10: 0f 94 c0                sete   al
  // 13: 48 c1 e0 07             shl    rax,0x7
  // 17: 48 0d 1f 00 00 00       or     rax,0x1f
  // -> if
  // 1d: 48 3d 1f 00 00 00       cmp    rax,0x1f
  // 23: 0f 84 19 00 00 00       je     0x42
  // +
  // 29: b8 08 00 00 00          mov    eax,0x8
  // 2e: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 33: b8 04 00 00 00          mov    eax,0x4
  // 38: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 3d: e9 14 00 00 00          jmp    0x56
  // +
  // 42: b8 10 00 00 00          mov    eax,0x10
  // 47: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 4c: b8 0c 00 00 00          mov    eax,0xc
  // 51: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 56: c3                      ret
  byte expected[] = {0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x3d, 0x00, 0x00, 0x00,
                     0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x94, 0xc0, 0x48,
                     0xc1, 0xe0, 0x07, 0x48, 0x0d, 0x1f, 0x00, 0x00, 0x00, 0x48,
                     0x3d, 0x1f, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x19, 0x00, 0x00,
                     0x00, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
                     0xf8, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x03, 0x44, 0x24,
                     0xf8, 0xe9, 0x14, 0x00, 0x00, 0x00, 0xb8, 0x10, 0x00, 0x00,
                     0x00, 0x48, 0x89, 0x44, 0x24, 0xf8, 0xb8, 0x0c, 0x00, 0x00,
                     0x00, 0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(3));
}

TEST(compile_if_test_false) {
  // (if (zero? 1) (+ 1 2) (+ 3 4))
  ASTNode *node =
      list4(AST_new_atom("if"), list2(AST_new_atom("zero?"), AST_new_fixnum(1)),
            list3(AST_new_atom("+"), AST_new_fixnum(1), AST_new_fixnum(2)),
            list3(AST_new_atom("+"), AST_new_fixnum(3), AST_new_fixnum(4)));
  int result = AST_compile_function(ctx, node);
  cmp_ok(result, "==", 0, __func__);
  // 0:  b8 04 00 00 00          mov    eax,imm(0x1)
  // -> zero?
  // 5:  48 3d 00 00 00 00       cmp    rax,0x0
  // b:  b8 00 00 00 00          mov    eax,0x0
  // 10: 0f 94 c0                sete   al
  // 13: 48 c1 e0 07             shl    rax,0x7
  // 17: 48 0d 1f 00 00 00       or     rax,0x1f
  // -> if
  // 1d: 48 3d 1f 00 00 00       cmp    rax,0x1f
  // 23: 0f 84 19 00 00 00       je     0x42
  // +
  // 29: b8 08 00 00 00          mov    eax,0x8
  // 2e: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 33: b8 04 00 00 00          mov    eax,0x4
  // 38: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 3d: e9 14 00 00 00          jmp    0x56
  // +
  // 42: b8 10 00 00 00          mov    eax,0x10
  // 47: 48 89 44 24 f8          mov    QWORD PTR [rsp-0x8],rax
  // 4c: b8 0c 00 00 00          mov    eax,0xc
  // 51: 48 03 44 24 f8          add    rax,QWORD PTR [rsp-0x8]
  // 56: c3                      ret
  byte expected[] = {0xb8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x3d, 0x00, 0x00, 0x00,
                     0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x94, 0xc0, 0x48,
                     0xc1, 0xe0, 0x07, 0x48, 0x0d, 0x1f, 0x00, 0x00, 0x00, 0x48,
                     0x3d, 0x1f, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x19, 0x00, 0x00,
                     0x00, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
                     0xf8, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x03, 0x44, 0x24,
                     0xf8, 0xe9, 0x14, 0x00, 0x00, 0x00, 0xb8, 0x10, 0x00, 0x00,
                     0x00, 0x48, 0x89, 0x44, 0x24, 0xf8, 0xb8, 0x0c, 0x00, 0x00,
                     0x00, 0x48, 0x03, 0x44, 0x24, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(7));
}

TEST(return_heap_address) {
  Buffer_mov_reg_reg(ctx->writer, /*dst=*/kRax, /*src=*/kRdi);
  Buffer_ret(ctx->writer);
  byte expected[] = {0x48, 0x89, 0xf8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  uint64_t result = Testing_call_entry(ctx->writer->buf, 0xdeadbeef);
  cmp_ok(result, "==", 0xdeadbeef, __func__);
}

TEST(compile_cons) {
  // (cons 10 20)
  ASTNode *node =
      list3(AST_new_atom("cons"), AST_new_fixnum(10), AST_new_fixnum(20));
  int compile_result = AST_compile_entry(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  // -> prologue
  // 0:  48 89 fe                mov    rsi,rdi
  // 3:  b8 28 00 00 00          mov    eax,0x28
  // 8:  48 89 46 00             mov    QWORD PTR [rsi+0x0],rax
  // c:  b8 50 00 00 00          mov    eax,0x50
  // 11: 48 89 46 08             mov    QWORD PTR [rsi+0x8],rax
  // 15: 48 89 f0                mov    rax,rsi
  // 18: 48 0d 01 00 00 00       or     rax,0x1
  // 1e: 81 c6 10 00 00 00       add    esi,0x10
  // 24: c3                      ret
  byte expected[] = {0x48, 0x89, 0xfe, 0xb8, 0x28, 0x00, 0x00, 0x00, 0x48, 0x89,
                     0x46, 0x00, 0xb8, 0x50, 0x00, 0x00, 0x00, 0x48, 0x89, 0x46,
                     0x08, 0x48, 0x89, 0xf0, 0x48, 0x0d, 0x01, 0x00, 0x00, 0x00,
                     0x81, 0xc6, 0x10, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  uint64_t result = Testing_call_entry(ctx->writer->buf, heap);
  cmp_ok(result, "==", (uintptr_t)heap | 0x1UL, __func__);
}

TEST(compile_car) {
  // (car (cons 10 20))
  ASTNode *node =
      list2(AST_new_atom("car"), list3(AST_new_atom("cons"), AST_new_fixnum(10),
                                       AST_new_fixnum(20)));
  int compile_result = AST_compile_entry(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  // -> prologue
  // 0:  48 89 fe                mov    rsi,rdi
  // -> cons
  // 3:  b8 28 00 00 00          mov    eax,0x28
  // 8:  48 89 46 00             mov    QWORD PTR [rsi+0x0],rax
  // c:  b8 50 00 00 00          mov    eax,0x50
  // 11: 48 89 46 08             mov    QWORD PTR [rsi+0x8],rax
  // 15: 48 89 f0                mov    rax,rsi
  // 18: 48 0d 01 00 00 00       or     rax,0x1
  // 1e: 81 c6 10 00 00 00       add    esi,0x10
  // -> car
  // 24: 48 8b 40 ff             mov    rax,QWORD PTR [rax-0x1]
  // 28: c3                      ret
  byte expected[] = {0x48, 0x89, 0xfe, 0xb8, 0x28, 0x00, 0x00, 0x00, 0x48,
                     0x89, 0x46, 0x00, 0xb8, 0x50, 0x00, 0x00, 0x00, 0x48,
                     0x89, 0x46, 0x08, 0x48, 0x89, 0xf0, 0x48, 0x0d, 0x01,
                     0x00, 0x00, 0x00, 0x81, 0xc6, 0x10, 0x00, 0x00, 0x00,
                     0x48, 0x8b, 0x40, 0xff, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  uint64_t result = Testing_call_entry(ctx->writer->buf, heap);
  cmp_ok(result, "==", encodeImmediateFixnum(10), __func__);
}

TEST(compile_cdr) {
  // (cdr (cons 10 20))
  ASTNode *node =
      list2(AST_new_atom("cdr"), list3(AST_new_atom("cons"), AST_new_fixnum(10),
                                       AST_new_fixnum(20)));
  int compile_result = AST_compile_entry(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  // -> prologue
  // 0:  48 89 fe                mov    rsi,rdi
  // -> cons
  // 3:  b8 28 00 00 00          mov    eax,0x28
  // 8:  48 89 46 00             mov    QWORD PTR [rsi+0x0],rax
  // c:  b8 50 00 00 00          mov    eax,0x50
  // 11: 48 89 46 08             mov    QWORD PTR [rsi+0x8],rax
  // 15: 48 89 f0                mov    rax,rsi
  // 18: 48 0d 01 00 00 00       or     rax,0x1
  // 1e: 81 c6 10 00 00 00       add    esi,0x10
  // -> cdr
  // 24: 48 8b 40 07             mov    rax,QWORD PTR [rax+0x7]
  // 28: c3                      ret
  byte expected[] = {0x48, 0x89, 0xfe, 0xb8, 0x28, 0x00, 0x00, 0x00, 0x48,
                     0x89, 0x46, 0x00, 0xb8, 0x50, 0x00, 0x00, 0x00, 0x48,
                     0x89, 0x46, 0x08, 0x48, 0x89, 0xf0, 0x48, 0x0d, 0x01,
                     0x00, 0x00, 0x00, 0x81, 0xc6, 0x10, 0x00, 0x00, 0x00,
                     0x48, 0x8b, 0x40, 0x07, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  uint64_t result = Testing_call_entry(ctx->writer->buf, heap);
  cmp_ok(result, "==", encodeImmediateFixnum(20), __func__);
}

TEST(compile_empty_labels) {
  // (labels () 5)
  ASTNode *prog = list3(AST_new_atom("labels"), nil, AST_new_fixnum(5));
  int compile_result = AST_compile_prog(ctx, prog);
  cmp_ok(compile_result, "==", 0, __func__);
  // jump 0x0; mov rsi, rdi; mov eax, imm(5); ret
  byte expected[] = {0xe9, 0x00, 0x00, 0x00, 0x00,
                     0x48, 0x89, 0xfe, 0xb8, encodeImmediateFixnum(5),
                     0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
}

TEST(compile_code_with_no_params) {
  // (code () 5)
  ASTNode *node = list3(AST_new_atom("code"), nil, AST_new_fixnum(5));
  int compile_result = AST_compile_function(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  // mov eax, imm(5); ret
  byte expected[] = {0xb8, encodeImmediateFixnum(5), 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(compile_code_with_params) {
  // (code (x y) (+ x y))
  ASTNode *node =
      list3(AST_new_atom("code"), list2(AST_new_atom("x"), AST_new_atom("y")),
            list3(AST_new_atom("+"), AST_new_atom("x"), AST_new_atom("y")));
  int compile_result = AST_compile_function(ctx, node);
  cmp_ok(compile_result, "==", 0, __func__);
  // -> Load formal (y) into a new temporary stack location (rsp-0x18)
  // 0:  48 8b 44 24 f0          mov    rax,QWORD PTR [rsp-0x10]
  // 5:  48 89 44 24 e8          mov    QWORD PTR [rsp-0x18],rax
  // -> Load formal (x) into rax
  // a:  48 8b 44 24 f8          mov    rax,QWORD PTR [rsp-0x8]
  // -> add
  // f:  48 03 44 24 e8          add    rax,QWORD PTR [rsp-0x18]
  // 14: c3                      ret
  byte expected[] = {0x48, 0x8b, 0x44, 0x24, 0xf0, 0x48, 0x89,
                     0x44, 0x24, 0xe8, 0x48, 0x8b, 0x44, 0x24,
                     0xf8, 0x48, 0x03, 0x44, 0x24, 0xe8, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
}

TEST(compile_label_with_no_param_and_no_labelcall) {
  // (labels ((const (code () 6))) 5)
  ASTNode *labels =
      list1(list2(AST_new_atom("const"),
                  list3(AST_new_atom("code"), nil, AST_new_fixnum(6))));
  ASTNode *prog = list3(AST_new_atom("labels"), labels, AST_new_fixnum(5));
  int compile_result = AST_compile_prog(ctx, prog);
  cmp_ok(compile_result, "==", 0, __func__);
  // -> jump to body
  // 0:  e9 06 00 00 00          jmp    0xb
  // -> code for (code () 5)
  // 5:  b8 18 00 00 00          mov    eax,0x18
  // a:  c3                      ret
  // -> body:
  // b:  48 89 fe                mov    rsi,rdi
  // e:  b8 14 00 00 00          mov    eax,0x14
  // 13: c3                      ret
  byte expected[] = {
      0xe9, 0x06, 0x00, 0x00, 0x00, 0xb8, encodeImmediateFixnum(6), 0x00, 0x00,
      0x00, 0xc3, 0x48, 0x89, 0xfe, 0xb8, encodeImmediateFixnum(5), 0x00, 0x00,
      0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
}

TEST(compile_labelcall_with_undefined_name) {
  ASTNode *prog =
      list2(AST_new_atom("labelcall"), AST_new_atom("nonexistent-label"));
  int compile_result = AST_compile_function(ctx, prog);
  cmp_ok(compile_result, "==", -1, __func__);
}

TEST(compile_labelcall_with_no_param) {
  // (labels ((const (code () 5))) (labelcall const))
  ASTNode *labels =
      list1(list2(AST_new_atom("const"),
                  list3(AST_new_atom("code"), nil, AST_new_fixnum(5))));
  ASTNode *prog =
      list3(AST_new_atom("labels"), labels,
            list2(AST_new_atom("labelcall"), AST_new_atom("const")));
  int compile_result = AST_compile_prog(ctx, prog);
  cmp_ok(compile_result, "==", 0, __func__);
  // 0:  e9 06 00 00 00          jmp    0xb
  // 5:  b8 14 00 00 00          mov    eax,0x14
  // a:  c3                      ret
  // b:  48 89 fe                mov    rsi,rdi
  // e:  e8 f2 ff ff ff          call   0x5
  // 13: c3                      ret
  byte expected[] = {0xe9, 0x06, 0x00, 0x00, 0x00, 0xb8, 0x14,
                     0x00, 0x00, 0x00, 0xc3, 0x48, 0x89, 0xfe,
                     0xe8, 0xf2, 0xff, 0xff, 0xff, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
}

TEST(compile_labelcall_with_one_param) {
  // (labels ((id (code (x) x))) (labelcall id 5))
  ASTNode *labels = list1(list2(
      AST_new_atom("id"), list3(AST_new_atom("code"), list1(AST_new_atom("x")),
                                AST_new_atom("x"))));
  ASTNode *prog = list3(
      AST_new_atom("labels"), labels,
      list3(AST_new_atom("labelcall"), AST_new_atom("id"), AST_new_fixnum(5)));
  int compile_result = AST_compile_prog(ctx, prog);
  cmp_ok(compile_result, "==", 0, __func__);
  // 0:  e9 06 00 00 00          jmp    0xb
  // 5:  48 8b 44 24 f8          mov    rax,QWORD PTR [rsp-0x8]
  // a:  c3                      ret
  // b:  48 89 fe                mov    rsi,rdi
  // e:  b8 14 00 00 00          mov    eax,0x14
  // 13: c3                      ret
  byte expected[] = {0xe9, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8b,
                     0x44, 0x24, 0xf8, 0xc3, 0x48, 0x89, 0xfe,
                     0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
  Buffer_make_executable(ctx->writer->buf);
  EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
}

// TEST(compile_labelcall_with_two_params) {
//   // (labels ((add (code (x y) (+ x y)))) (labelcall add 3 4))
//   ASTNode *body =
//       list3(AST_new_atom("+"), AST_new_atom("x"), AST_new_atom("y"));
//   ASTNode *labels =
//       list1(list2(AST_new_atom("id"),
//                   list3(AST_new_atom("code"),
//                         list2(AST_new_atom("x"), AST_new_atom("y")), body)));
//   ASTNode *prog = list3(AST_new_atom("labels"), labels, AST_new_fixnum(5));
//   int compile_result = AST_compile_prog(ctx, prog);
//   cmp_ok(compile_result, "==", 0, __func__);
//   // 0:  e9 06 00 00 00          jmp    0xb
//   // 5:  48 8b 44 24 f8          mov    rax,QWORD PTR [rsp-0x8]
//   // a:  c3                      ret
//   // b:  48 89 fe                mov    rsi,rdi
//   // e:  b8 14 00 00 00          mov    eax,0x14
//   // 13: c3                      ret
//   byte expected[] = {0xe9, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8b,
//                      0x44, 0x24, 0xf8, 0xc3, 0x48, 0x89, 0xfe,
//                      0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};
//   EXPECT_EQUALS_BYTES(ctx->writer->buf, expected);
//   Buffer_make_executable(ctx->writer->buf);
//   EXPECT_CALL_EQUALS(ctx->writer->buf, encodeImmediateFixnum(5));
// }

TEST(read_with_number_returns_fixnum) {
  (void)ctx;
  char *input = "1234";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kFixnum);
  cmp_ok(output->value.fixnum, "==", 1234);
}

TEST(read_with_leading_whitespace_ignores_whitespace) {
  (void)ctx;
  char *input = "  \t \n 1234";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kFixnum);
  cmp_ok(output->value.fixnum, "==", 1234);
}

TEST(read_with_atom_returns_atom) {
  (void)ctx;
  char *input = "hello";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kAtom);
  ok(AST_atom_equals_cstr(output, "hello"));
}

TEST(read_with_nil_returns_nil) {
  (void)ctx;
  char *input = "()";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kCons);
  ok(output == nil);
}

TEST(read_with_list_returns_list) {
  (void)ctx;
  char *input = "(1 2 3)";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kCons);
  ASTNode *elt = AST_car(output);
  cmp_ok(elt->type, "==", kFixnum);
  cmp_ok(elt->value.fixnum, "==", 1);
  elt = AST_car(AST_cdr(output));
  cmp_ok(elt->type, "==", kFixnum);
  cmp_ok(elt->value.fixnum, "==", 2);
  elt = AST_car(AST_cdr(AST_cdr(output)));
  cmp_ok(elt->type, "==", kFixnum);
  cmp_ok(elt->value.fixnum, "==", 3);
}

TEST(read_with_nested_list_returns_list) {
  (void)ctx;
  char *input = "((hello world) (foo bar))";
  ASTNode *output = Reader_read(input);
  assert(output != NULL);
  cmp_ok(output->type, "==", kCons);
  ASTNode *elt = AST_car(output);
  cmp_ok(elt->type, "==", kCons);
  ok(AST_atom_equals_cstr(AST_car(elt), "hello"));
  ok(AST_atom_equals_cstr(AST_car(AST_cdr(elt)), "world"));
  elt = AST_car(AST_cdr(output));
  cmp_ok(elt->type, "==", kCons);
  ok(AST_atom_equals_cstr(AST_car(elt), "foo"));
  ok(AST_atom_equals_cstr(AST_car(AST_cdr(elt)), "bar"));
}

TEST(compile_with_read) {
  uint64_t result = Run_from_cstr("(let ((x 2) (y 3)) (+ x y))", ctx, heap);
  cmp_ok(result, "==", encodeImmediateFixnum(5), __func__);
}

int run_tests() {
  plan(NO_PLAN);
  run_test(test_write_bytes_manually);
  run_test(test_write_bytes_manually2);
  run_test(test_mov_rax_imm32);
  run_test(test_mov_rcx_imm32);
  run_test(test_mov_inc);
  run_test(test_mov_rax_rax);
  run_test(test_mov_rax_rsi);
  run_test(test_mov_rdi_rbp);
  run_test(test_compile_fixnum);
  run_test(test_compile_primcall_add1);
  run_test(test_compile_primcall_sub1);
  run_test(test_compile_primcall_add1_sub1);
  run_test(test_compile_primcall_sub1_add1);
  run_test(test_compile_add_two_ints);
  run_test(test_compile_add_three_ints);
  run_test(test_compile_add_four_ints);
  run_test(test_integer_to_char);
  run_test(test_zerop_with_zero_returns_true);
  run_test(test_zerop_with_non_zero_returns_false);
  run_test(test_let_with_no_bindings);
  run_test(test_let_with_one_binding);
  run_test(test_compile_atom_with_undefined_variable);
  run_test(test_compile_atom_in_env_emits_stack_index);
  run_test(test_compile_if_test_true);
  run_test(test_compile_if_test_false);
  run_test(test_return_heap_address);
  run_test(test_compile_cons);
  run_test(test_compile_car);
  run_test(test_compile_cdr);
  run_test(test_compile_empty_labels);
  run_test(test_compile_code_with_no_params);
  run_test(test_compile_code_with_params);
  run_test(test_compile_label_with_no_param_and_no_labelcall);
  run_test(test_compile_labelcall_with_undefined_name);
  run_test(test_compile_labelcall_with_no_param);
  run_test(test_compile_labelcall_with_one_param);
  run_test(test_read_with_number_returns_fixnum);
  run_test(test_read_with_leading_whitespace_ignores_whitespace);
  run_test(test_read_with_atom_returns_atom);
  run_test(test_read_with_nil_returns_nil);
  run_test(test_read_with_list_returns_list);
  run_test(test_read_with_nested_list_returns_list);
  run_test(test_compile_with_read);
  done_testing();
}

// End Testing

int main() { return run_tests(); }
