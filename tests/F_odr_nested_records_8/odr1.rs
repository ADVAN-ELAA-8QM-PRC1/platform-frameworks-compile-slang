#pragma version(1)

#pragma rs java_package_name(com.android.rs.test)

struct Inner {
  uint32_t x;
};

// Mismatching array size of struct Inner
struct Outer {
  struct Inner current[2];
};

extern uint32_t uint32_ret;

extern struct Outer *outer;

void outer_y(void) {
  uint32_ret = outer->current[1].x;
}

