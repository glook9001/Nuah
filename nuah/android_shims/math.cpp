#include <dlfcn.h>

namespace {
template <typename Function>
Function host(const char* name) {
  return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, name));
}
}

#define NUAH_UNARY_DOUBLE(name) \
  extern "C" double name(double value) { \
    return host<double (*)(double)>(#name)(value); \
  }
#define NUAH_UNARY_FLOAT(name) \
  extern "C" float name(float value) { \
    return host<float (*)(float)>(#name)(value); \
  }
#define NUAH_BINARY_DOUBLE(name) \
  extern "C" double name(double left, double right) { \
    return host<double (*)(double, double)>(#name)(left, right); \
  }
#define NUAH_BINARY_FLOAT(name) \
  extern "C" float name(float left, float right) { \
    return host<float (*)(float, float)>(#name)(left, right); \
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

extern "C" double frexp(double value, int* exponent) {
  return host<double (*)(double, int*)>("frexp")(value, exponent);
}
extern "C" float frexpf(float value, int* exponent) {
  return host<float (*)(float, int*)>("frexpf")(value, exponent);
}
extern "C" double ldexp(double value, int exponent) {
  return host<double (*)(double, int)>("ldexp")(value, exponent);
}
extern "C" float ldexpf(float value, int exponent) {
  return host<float (*)(float, int)>("ldexpf")(value, exponent);
}
extern "C" double modf(double value, double* integral) {
  return host<double (*)(double, double*)>("modf")(value, integral);
}
extern "C" float modff(float value, float* integral) {
  return host<float (*)(float, float*)>("modff")(value, integral);
}
extern "C" void sincos(double value, double* sine, double* cosine) {
  host<void (*)(double, double*, double*)>("sincos")(value, sine, cosine);
}
extern "C" void sincosf(float value, float* sine, float* cosine) {
  host<void (*)(float, float*, float*)>("sincosf")(value, sine, cosine);
}
