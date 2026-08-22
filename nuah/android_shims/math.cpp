#include <dlfcn.h>

#include "nuah/android_abi_registry.h"

namespace {
template <typename Function>
Function host(const char* name) {
  return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, name));
}
}

#define NUAH_UNARY_DOUBLE(name) \
  extern "C" double name(double value) { \
    static const auto fn = \
        reinterpret_cast<double (*)(double)>(::dlsym(RTLD_NEXT, #name)); \
    return fn(value); \
  }
#define NUAH_UNARY_FLOAT(name) \
  extern "C" float name(float value) { \
    static const auto fn = \
        reinterpret_cast<float (*)(float)>(::dlsym(RTLD_NEXT, #name)); \
    return fn(value); \
  }
#define NUAH_BINARY_DOUBLE(name) \
  extern "C" double name(double left, double right) { \
    static const auto fn = \
        reinterpret_cast<double (*)(double, double)>(::dlsym(RTLD_NEXT, #name)); \
    return fn(left, right); \
  }
#define NUAH_BINARY_FLOAT(name) \
  extern "C" float name(float left, float right) { \
    static const auto fn = \
        reinterpret_cast<float (*)(float, float)>(::dlsym(RTLD_NEXT, #name)); \
    return fn(left, right); \
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
  static const auto fn =
      reinterpret_cast<int (*)(float)>(::dlsym(RTLD_NEXT, "finitef"));
  return fn(value);
}
extern "C" long double fmal(long double left, long double right,
                             long double addend) {
  static const auto fn = reinterpret_cast<long double (*)(
      long double, long double, long double)>(::dlsym(RTLD_NEXT, "fmal"));
  return fn(left, right, addend);
}
extern "C" int ilogb(double value) {
  static const auto fn =
      reinterpret_cast<int (*)(double)>(::dlsym(RTLD_NEXT, "ilogb"));
  return fn(value);
}
extern "C" long long llround(double value) {
  static const auto fn =
      reinterpret_cast<long long (*)(double)>(::dlsym(RTLD_NEXT, "llround"));
  return fn(value);
}
extern "C" long long llroundf(float value) {
  static const auto fn =
      reinterpret_cast<long long (*)(float)>(::dlsym(RTLD_NEXT, "llroundf"));
  return fn(value);
}
extern "C" long lround(double value) {
  static const auto fn =
      reinterpret_cast<long (*)(double)>(::dlsym(RTLD_NEXT, "lround"));
  return fn(value);
}
extern "C" long lroundf(float value) {
  static const auto fn =
      reinterpret_cast<long (*)(float)>(::dlsym(RTLD_NEXT, "lroundf"));
  return fn(value);
}
extern "C" double nan(const char* tag) {
  static const auto fn =
      reinterpret_cast<double (*)(const char*)>(::dlsym(RTLD_NEXT, "nan"));
  return fn(tag);
}
extern "C" long double powl(long double left, long double right) {
  static const auto fn = reinterpret_cast<long double (*)(
      long double, long double)>(::dlsym(RTLD_NEXT, "powl"));
  return fn(left, right);
}
extern "C" float remquof(float left, float right, int* quotient) {
  static const auto fn = reinterpret_cast<float (*)(float, float, int*)>(
      ::dlsym(RTLD_NEXT, "remquof"));
  return fn(left, right, quotient);
}

extern "C" double frexp(double value, int* exponent) {
  static const auto fn =
      reinterpret_cast<double (*)(double, int*)>(::dlsym(RTLD_NEXT, "frexp"));
  return fn(value, exponent);
}
extern "C" float frexpf(float value, int* exponent) {
  static const auto fn =
      reinterpret_cast<float (*)(float, int*)>(::dlsym(RTLD_NEXT, "frexpf"));
  return fn(value, exponent);
}
extern "C" double ldexp(double value, int exponent) {
  static const auto fn =
      reinterpret_cast<double (*)(double, int)>(::dlsym(RTLD_NEXT, "ldexp"));
  return fn(value, exponent);
}
extern "C" float ldexpf(float value, int exponent) {
  static const auto fn =
      reinterpret_cast<float (*)(float, int)>(::dlsym(RTLD_NEXT, "ldexpf"));
  return fn(value, exponent);
}
extern "C" double modf(double value, double* integral) {
  static const auto fn =
      reinterpret_cast<double (*)(double, double*)>(::dlsym(RTLD_NEXT, "modf"));
  return fn(value, integral);
}
extern "C" float modff(float value, float* integral) {
  static const auto fn =
      reinterpret_cast<float (*)(float, float*)>(::dlsym(RTLD_NEXT, "modff"));
  return fn(value, integral);
}
extern "C" void sincos(double value, double* sine, double* cosine) {
  static const auto fn = reinterpret_cast<void (*)(double, double*, double*)>(
      ::dlsym(RTLD_NEXT, "sincos"));
  fn(value, sine, cosine);
}
extern "C" void sincosf(float value, float* sine, float* cosine) {
  static const auto fn = reinterpret_cast<void (*)(float, float*, float*)>(
      ::dlsym(RTLD_NEXT, "sincosf"));
  fn(value, sine, cosine);
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
