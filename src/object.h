#ifndef clox_object_h
#define clox_object_h

#include "common.h"
#include "value.h"

typedef enum {
  OBJ_STRING,
} ObjectType;

struct Object {
  ObjectType type;
  // Whether the object's underlying memory is owned by the VM.
  // Heap allocations from Object are always owned by the VM and
  // should be GC'd, but data referenced internally by the object may
  // not be.
  //
  // Ex: strings and (in the future) ffi objects / C function pointers.
  //
  // Can have different behavior depending on the object type.
  //
  // For strings, this means that the string's `chars` array is owned by the
  // VM and should be freed when the wrapping ObjectString is freed.
  bool owned;
  struct Object *next;
};

// TODO: use [flexible array
// members](https://en.wikipedia.org/wiki/Flexible_array_member) to reduce
// indirection.
//
// TODO: implement utf-8 strings, with codepoint indexing. This will require
// some changes to the scanner and string literal parsing.
struct ObjectString {
  Object object;
  size_t length;
  uint32_t hash;
  char *chars;
};

static inline bool is_obj_type(Value value, ObjectType type) {
  return (IS_OBJ(value) && OBJ_TYPE(value) == type);
}

#define IS_STRING(value) is_obj_type(value, OBJ_STRING)

#define AS_STRING(value) ((ObjectString *)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjectString *)AS_OBJ(value))->chars)

ObjectString *copy_string(const char *start, size_t length);
ObjectString *take_string(char *chars, size_t length);
ObjectString *ref_string(char *chars, size_t length);

void object_print(Value obj);

#endif
