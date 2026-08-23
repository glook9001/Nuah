#include <cmath>

#include "nuah/android_abi_registry.h"

#define NUAH_UNARY_DOUBLE(name) \
  extern "C" double name(double value) { \
    return __builtin_##name(value); \
  }
#define NUAH_UNARY_FLOAT(name) \
  extern "C" float name(float value) { \
    return __builtin_##name(value); \
  }
#define NUAH_BINARY_DOUBLE(name) \
  extern "C" double name(double left, double right) { \
    return __builtin_##name(left, right); \
  }
#define NUAH_BINARY_FLOAT(name) \
  extern "C" float name(float left, float right) { \
    return __builtin_##name(left, right); \
  }

NUAH_UNARY_DOUBLE(acos)
NUAH_UNARY_DOUBLE(asin)
NUAH_UNARY_DOUBLE(atan)
NUAH_UNARY_DOUBLE(cbrt)
NUAH_UNARY_DOUBLE(cos)
NUAH_UNARY_DOUBLE(cosh)
NUAH_UNARY_DOUBLE(exp)
NUAH_UNARY_DOUBLE(exp2)
NUAH_UNARY_DOUBLE(expm1)
NUAH_UNARY_DOUBLE(log)
NUAH_UNARY_DOUBLE(log10)
NUAH_UNARY_DOUBLE(log2)
NUAH_UNARY_DOUBLE(sin)
NUAH_UNARY_DOUBLE(sinh)
NUAH_UNARY_DOUBLE(sqrt)
NUAH_UNARY_DOUBLE(tan)
NUAH_UNARY_DOUBLE(tanh)
NUAH_UNARY_DOUBLE(round)
NUAH_UNARY_FLOAT(acosf)
NUAH_UNARY_FLOAT(asinf)
NUAH_UNARY_FLOAT(atanf)
NUAH_UNARY_FLOAT(cbrtf)
NUAH_UNARY_FLOAT(cosf)
NUAH_UNARY_FLOAT(coshf)
NUAH_UNARY_FLOAT(expf)
NUAH_UNARY_FLOAT(exp2f)
NUAH_UNARY_FLOAT(erfcf)
NUAH_UNARY_FLOAT(erff)
NUAH_UNARY_FLOAT(logf)
NUAH_UNARY_FLOAT(log10f)
NUAH_UNARY_FLOAT(log2f)
NUAH_UNARY_FLOAT(sinf)
NUAH_UNARY_FLOAT(sinhf)
NUAH_UNARY_FLOAT(sqrtf)
NUAH_UNARY_FLOAT(tanf)
NUAH_UNARY_FLOAT(tanhf)
NUAH_BINARY_DOUBLE(atan2)
NUAH_BINARY_DOUBLE(fmod)
NUAH_BINARY_DOUBLE(pow)
NUAH_BINARY_FLOAT(atan2f)
NUAH_BINARY_FLOAT(fmodf)
NUAH_BINARY_FLOAT(powf)
NUAH_BINARY_FLOAT(nextafterf)
NUAH_BINARY_FLOAT(remainderf)

extern "C" int finitef(float value) {
  return std::isfinite(value) ? 1 : 0;
}
extern "C" long double fmal(long double left, long double right,
                             long double addend) {
  return __builtin_fmal(left, right, addend);
}
extern "C" int ilogb(double value) {
  return __builtin_ilogb(value);
}
extern "C" long long llround(double value) {
  return __builtin_llround(value);
}
extern "C" long long llroundf(float value) {
  return __builtin_llroundf(value);
}
extern "C" long lround(double value) {
  return __builtin_lround(value);
}
extern "C" long lroundf(float value) {
  return __builtin_lroundf(value);
}
extern "C" double nan(const char* tag) {
  return __builtin_nan(tag);
}
extern "C" long double powl(long double left, long double right) {
  return __builtin_powl(left, right);
}
extern "C" float remquof(float left, float right, int* quotient) {
  return __builtin_remquof(left, right, quotient);
}

extern "C" double frexp(double value, int* exponent) {
  return __builtin_frexp(value, exponent);
}
extern "C" float frexpf(float value, int* exponent) {
  return __builtin_frexpf(value, exponent);
}
extern "C" double ldexp(double value, int exponent) {
  return __builtin_ldexp(value, exponent);
}
extern "C" float ldexpf(float value, int exponent) {
  return __builtin_ldexpf(value, exponent);
}
extern "C" double modf(double value, double* integral) {
  return __builtin_modf(value, integral);
}
extern "C" float modff(float value, float* integral) {
  return __builtin_modff(value, integral);
}
extern "C" void sincos(double value, double* sine, double* cosine) {
  __builtin_sincos(value, sine, cosine);
}
extern "C" void sincosf(float value, float* sine, float* cosine) {
  __builtin_sincosf(value, sine, cosine);
}

__attribute__((constructor)) static void register_math_abi() {
  static constexpr const char* symbols[] = {
      "acos",   "acosf",  "asin",   "asinf",  "atan",   "atanf",
      "atan2",  "atan2f", "cbrt",   "cbrtf",  "cos",    "cosf",
      "cosh",   "coshf",  "exp",    "exp2",   "exp2f",  "expf",
      "expm1",  "erfcf",  "erff",   "finitef", "fmal",  "fmod",
      "fmodf",  "frexp",  "frexpf", "ilogb",  "ldexp",
      "ldexpf", "log",    "log10",  "log10f", "log2",   "log2f",
      "logf",   "llround", "llroundf", "lround", "lroundf", "modf",
      "modff",  "nan",    "nextafterf", "pow", "powf", "powl",
      "remainderf", "remquof", "round", "sin", "sincos", "sincosf",
      "sinf",   "sinh",   "sinhf",  "sqrt",   "sqrtf",  "tan",
      "tanf",   "tanh",   "tanhf"};
  for (const char* symbol : symbols) {
    nuah_android_api_register("libm.so", symbol,
                              NUAH_ANDROID_API_TRANSLATED);
  }
}
