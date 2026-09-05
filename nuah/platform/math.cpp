#include <cmath>
#include "nuah/android_abi_registry.h"
#define NUAH_UNARY_DOUBLE(name) extern "C" double name(double v) { return __builtin_##name(v); }
#define NUAH_UNARY_FLOAT(name) extern "C" float name(float v) { return __builtin_##name(v); }
#define NUAH_BINARY_DOUBLE(name) extern "C" double name(double l, double r) { return __builtin_##name(l, r); }
#define NUAH_BINARY_FLOAT(name) extern "C" float name(float l, float r) { return __builtin_##name(l, r); }
#define NUAH_DBL1(X) X(acos) X(asin) X(atan) X(cbrt) X(ceil) X(cos) X(cosh) X(exp) X(exp2) X(expm1) X(fabs) X(floor) X(log) X(log10) X(log2) X(round) X(sin) X(sinh) X(sqrt) X(tan) X(tanh) X(trunc)
#define NUAH_FLT1(X) X(acosf) X(asinf) X(atanf) X(cbrtf) X(ceilf) X(cosf) X(coshf) X(expf) X(exp2f) X(erfcf) X(erff) X(fabsf) X(floorf) X(logf) X(log10f) X(log2f) X(roundf) X(sinf) X(sinhf) X(sqrtf) X(tanf) X(tanhf) X(truncf)
#define NUAH_DBL2(X) X(atan2) X(copysign) X(fdim) X(fmax) X(fmin) X(fmod) X(hypot) X(nextafter) X(pow) X(remainder)
#define NUAH_FLT2(X) X(atan2f) X(copysignf) X(fdimf) X(fmaxf) X(fminf) X(fmodf) X(hypotf) X(nextafterf) X(powf) X(remainderf)
NUAH_DBL1(NUAH_UNARY_DOUBLE) NUAH_FLT1(NUAH_UNARY_FLOAT) NUAH_DBL2(NUAH_BINARY_DOUBLE) NUAH_FLT2(NUAH_BINARY_FLOAT)
#define NUAH_WRAP(ret, name, params, call) extern "C" ret name params { return call; }
NUAH_WRAP(double, fma, (double a, double b, double c), __builtin_fma(a, b, c)) NUAH_WRAP(float, fmaf, (float a, float b, float c), __builtin_fmaf(a, b, c)) NUAH_WRAP(long double, fmal, (long double a, long double b, long double c), __builtin_fmal(a, b, c)) NUAH_WRAP(double, frexp, (double v, int* e), __builtin_frexp(v, e)) NUAH_WRAP(float, frexpf, (float v, int* e), __builtin_frexpf(v, e))
NUAH_WRAP(double, ldexp, (double v, int e), __builtin_ldexp(v, e)) NUAH_WRAP(float, ldexpf, (float v, int e), __builtin_ldexpf(v, e)) NUAH_WRAP(double, modf, (double v, double* i), __builtin_modf(v, i)) NUAH_WRAP(float, modff, (float v, float* i), __builtin_modff(v, i))
NUAH_WRAP(int, ilogb, (double v), __builtin_ilogb(v)) NUAH_WRAP(long long, llround, (double v), __builtin_llround(v)) NUAH_WRAP(long long, llroundf, (float v), __builtin_llroundf(v))
NUAH_WRAP(long, lround, (double v), __builtin_lround(v)) NUAH_WRAP(long, lroundf, (float v), __builtin_lroundf(v)) NUAH_WRAP(double, nan, (const char* t), __builtin_nan(t)) NUAH_WRAP(long double, powl, (long double a, long double b), __builtin_powl(a, b)) NUAH_WRAP(float, remquof, (float a, float b, int* q), __builtin_remquof(a, b, q)) NUAH_WRAP(int, finitef, (float v), std::isfinite(v) ? 1 : 0)
extern "C" void sincos(double v, double* s, double* c) { __builtin_sincos(v, s, c); } extern "C" void sincosf(float v, float* s, float* c) { __builtin_sincosf(v, s, c); }
__attribute__((constructor)) static void register_math_abi() {
#define NUAH_SYM(n) nuah_android_api_register("libm.so", #n, NUAH_ANDROID_API_TRANSLATED);
  NUAH_DBL1(NUAH_SYM) NUAH_FLT1(NUAH_SYM) NUAH_DBL2(NUAH_SYM) NUAH_FLT2(NUAH_SYM)
  static constexpr const char* kExtra[] = {"fma", "fmaf", "fmal", "finitef", "frexp", "frexpf", "ilogb", "ldexp", "ldexpf", "llround", "llroundf", "lround", "lroundf", "modf", "modff", "nan", "powl", "remquof", "sincos", "sincosf"};
  for (const char* s : kExtra) nuah_android_api_register("libm.so", s, NUAH_ANDROID_API_TRANSLATED); }
