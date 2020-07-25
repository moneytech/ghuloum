#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef _GNU_SOURCE

#include "libtap/tap.h"

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

typedef struct {
  Buffer *buf;
  size_t pos;
} BufferWriter;

void BufferWriter_init(BufferWriter *writer, Buffer *buf) {
  writer->buf = buf;
  writer->pos = 0;
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

const int kBitsPerByte = 8;

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

void Buffer_mov_reg_imm64(BufferWriter *writer, Register dst, int64_t src) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0xb8 + dst);
  for (size_t i = 0; i < 8; i++) {
    Buffer_write8(writer, (src >> (i * kBitsPerByte)) & 0xff);
  }
  Buffer_write8(writer, 0x00);
  Buffer_write8(writer, 0x00);
  Buffer_write8(writer, 0x00);
}

void Buffer_mov_reg_reg(BufferWriter *writer, Register dst, Register src) {
  Buffer_write8(writer, 0x48);
  Buffer_write8(writer, 0x89);
  Buffer_write8(writer, 0xc0 + dst + src * 8);
}

void Buffer_ret(BufferWriter *writer) { Buffer_write8(writer, 0xc3); }

// End Machine code

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

typedef struct ASTNode {
  ASTNodeType type;
  union {
    int fixnum;
    char *atom;
    ASTCons cons;
  } value;
} ASTNode;

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
  assert(cons->type == kCons);
  return cons->value.cons.car;
}
ASTNode *AST_cdr(ASTNode *cons) {
  assert(cons->type == kCons);
  return cons->value.cons.cdr;
}

static const int kFixnumShift = 2;

int AST_compile_expr(BufferWriter *writer, ASTNode *node);

int AST_compile_call(BufferWriter *writer, ASTNode *car, ASTNode *cdr) {
  if (AST_is_atom(car)) {
    // Assumed to be a primcall
    if (AST_atom_equals_cstr(car, "add1")) {
      ASTNode *arg1 = AST_car(cdr);
      AST_compile_expr(writer, arg1);
      Buffer_add_reg_imm32(writer, kRax, 1 << kFixnumShift);
      return 0;
    }
    if (AST_atom_equals_cstr(car, "sub1")) {
      ASTNode *arg1 = AST_car(cdr);
      AST_compile_expr(writer, arg1);
      Buffer_sub_reg_imm32(writer, kRax, 1 << kFixnumShift);
      return 0;
    }
    assert(0 && "unknown call");
  }
  assert(0 && "unknown call");
}

int AST_compile_expr(BufferWriter *writer, ASTNode *node) {
  switch (node->type) {
  case kFixnum: {
    uint32_t value = (uint32_t)node->value.fixnum;
    uint32_t encoded = value << kFixnumShift;
    Buffer_mov_reg_imm32(writer, kRax, encoded);
    return 0;
  }
  case kCons: {
    // Assumed to be in the form (<expr> <op1> <op2> ...)
    return AST_compile_call(writer, AST_car(node), AST_cdr(node));
  }
  case kAtom:
    assert(0 && "unimplemented");
  }
  return -1;
}

int AST_compile_function(BufferWriter *writer, ASTNode *node) {
  int result = AST_compile_expr(writer, node);
  if (result != 0)
    return result;
  Buffer_ret(writer);
  return 0;
}

// End AST

// Testing

int call_intfunction(Buffer *buf) {
  assert(buf != NULL);
  assert(buf->address != NULL);
  assert(buf->state == kExecutable);
  int (*function)() = (int (*)())buf->address;
  return function();
}

void run_test(void (*test_body)(BufferWriter *)) {
  Buffer buf;
  Buffer_init(&buf, 100);
  {
    BufferWriter writer;
    BufferWriter_init(&writer, &buf);
    test_body(&writer);
  }
  Buffer_deinit(&buf);
}

#define EXPECT_EQUALS_BYTES(buf, arr)                                          \
  {                                                                            \
    int result =                                                               \
        cmp_ok(memcmp(buf->address, arr, sizeof arr), "==", 0, __func__);      \
    if (!result) {                                                             \
      printf("NOT EQUAL. Found: ");                                            \
      for (size_t i = 0; i < sizeof arr; i++) {                                \
        printf("%x ", buf->address[i]);                                        \
      }                                                                        \
      printf("\n");                                                            \
    }                                                                          \
  }

#define EXPECT_CALL_EQUALS(buf, expected)                                      \
  cmp_ok(call_intfunction(buf), "==", expected, __func__)

#define TEST(name) static void test_##name(BufferWriter *writer)

TEST(write_bytes_manually) {
  byte arr[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
  Buffer_write_arr(writer, arr, sizeof arr);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 42);
}

TEST(write_bytes_manually2) {
  byte arr[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xff, 0xc0, 0xc3};
  Buffer_write_arr(writer, arr, sizeof arr);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 43);
}

TEST(mov_rax_imm32) {
  Buffer_mov_reg_imm32(writer, kRax, 42);
  byte expected[] = {0xb8, 0x2a, 0x00, 0x00, 0x00};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
}

TEST(mov_rcx_imm32) {
  Buffer_mov_reg_imm32(writer, kRcx, 42);
  byte expected[] = {0xb9, 0x2a, 0x00, 0x00, 0x00};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
}

TEST(mov_inc) {
  Buffer_mov_reg_imm32(writer, kRax, 42);
  Buffer_inc_reg(writer, kRax);
  Buffer_ret(writer);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 43);
}

TEST(mov_rax_rax) {
  Buffer_mov_reg_reg(writer, /*dst=*/kRax, /*src=*/kRax);
  byte expected[] = {0x48, 0x89, 0xc0};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
}

TEST(mov_rax_rsi) {
  Buffer_mov_reg_reg(writer, /*dst=*/kRax, /*src=*/kRsi);
  byte expected[] = {0x48, 0x89, 0xf0};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
}

TEST(mov_rdi_rbp) {
  Buffer_mov_reg_reg(writer, /*dst=*/kRdi, /*src=*/kRbp);
  byte expected[] = {0x48, 0x89, 0xef};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
}

TEST(compile_fixnum) {
  // 123
  ASTNode *node = AST_new_fixnum(123);
  int result = AST_compile_function(writer, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, 123; ret
  byte expected[] = {0xb8, 0xec, 0x01, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 123 << kFixnumShift);
  free(node);
}

TEST(compile_primcall_add1) {
  // (add1 5)
  ASTNode *node =
      AST_new_cons(AST_new_atom("add1"), AST_new_cons(AST_new_fixnum(5), NULL));
  int result = AST_compile_function(writer, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); add eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x05,
                     0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 6 << kFixnumShift);
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_sub1) {
  // (sub1 5)
  ASTNode *node =
      AST_new_cons(AST_new_atom("sub1"), AST_new_cons(AST_new_fixnum(5), NULL));
  int result = AST_compile_function(writer, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); sub eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x2d,
                     0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 4 << kFixnumShift);
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_add1_sub1) {
  // (sub1 (add1 5))
  ASTNode *add1 =
      AST_new_cons(AST_new_atom("add1"), AST_new_cons(AST_new_fixnum(5), NULL));
  ASTNode *node = AST_new_cons(AST_new_atom("sub1"), AST_new_cons(add1, NULL));
  int result = AST_compile_function(writer, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); add eax, imm(1); sub eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00,
                     0x00, 0x00, 0x2d, 0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 5 << kFixnumShift);
  // TODO: figure out how to collect ASTs
}

TEST(compile_primcall_sub1_add1) {
  // (add1 (sub1 5))
  ASTNode *sub1 =
      AST_new_cons(AST_new_atom("sub1"), AST_new_cons(AST_new_fixnum(5), NULL));
  ASTNode *node = AST_new_cons(AST_new_atom("add1"), AST_new_cons(sub1, NULL));
  int result = AST_compile_function(writer, node);
  cmp_ok(result, "==", 0, __func__);
  // mov eax, imm(5); sub eax, imm(1); add eax, imm(1); ret
  byte expected[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0x2d, 0x04, 0x00,
                     0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0xc3};
  EXPECT_EQUALS_BYTES(writer->buf, expected);
  Buffer_make_executable(writer->buf);
  EXPECT_CALL_EQUALS(writer->buf, 5 << kFixnumShift);
  // TODO: figure out how to collect ASTs
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
  done_testing();
}

// End Testing

int main() { return run_tests(); }
