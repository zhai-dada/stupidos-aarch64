#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/*
 * stupidos 最小 math 兼容层。
 *
 * 目标不是做出高精度 libm，而是先满足 CPython 启动和常见浮点运算。
 * 这些实现优先保证：
 * - 不依赖宿主 glibc 的静态 libm
 * - 不触发 TLS / pthread 这类与当前用户态模型不一致的链接问题
 * - 逻辑简单、可维护，后续可以继续逐步替换成更完整的实现
 */

typedef union
{
    double d;
    uint64_t u;
} u_math_bits_t;

#define U_MATH_SIGN_MASK 0x8000000000000000ULL
#define U_MATH_ABS_MASK  0x7fffffffffffffffULL
#define U_MATH_EXP_MASK  0x7ff0000000000000ULL
#define U_MATH_MANT_MASK 0x000fffffffffffffULL

static inline bool u_math_is_nan_raw(double x)
{
    u_math_bits_t b;

    b.d = x;
    return ((b.u & U_MATH_EXP_MASK) == U_MATH_EXP_MASK) && ((b.u & U_MATH_MANT_MASK) != 0);
}

static inline bool u_math_is_inf_raw(double x)
{
    u_math_bits_t b;

    b.d = x;
    return (b.u & U_MATH_ABS_MASK) == U_MATH_EXP_MASK;
}

static inline double u_math_abs(double x)
{
    u_math_bits_t b;

    b.d = x;
    b.u &= U_MATH_ABS_MASK;
    return b.d;
}

static inline double u_math_copysign(double x, double y)
{
    u_math_bits_t bx;
    u_math_bits_t by;

    bx.d = x;
    by.d = y;
    bx.u = (bx.u & U_MATH_ABS_MASK) | (by.u & U_MATH_SIGN_MASK);
    return bx.d;
}

static inline double u_math_trunc(double x)
{
    long long i;

    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        return x;
    }

    i = (long long)x;
    return (double)i;
}

static inline double u_math_floor(double x)
{
    double t;

    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        return x;
    }

    t = u_math_trunc(x);
    if (t > x)
    {
        t -= 1.0;
    }
    return t;
}

static inline double u_math_ceil(double x)
{
    double t;

    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        return x;
    }

    t = u_math_trunc(x);
    if (t < x)
    {
        t += 1.0;
    }
    return t;
}

static inline double u_math_round_nearest(double x)
{
    if (x >= 0.0)
    {
        return u_math_floor(x + 0.5);
    }
    return u_math_ceil(x - 0.5);
}

static double u_math_fmod(double x, double y)
{
    double q;

    if (u_math_is_nan_raw(x) || u_math_is_nan_raw(y) || y == 0.0 || u_math_is_inf_raw(x))
    {
        return 0.0 / 0.0;
    }

    q = u_math_trunc(x / y);
    return x - q * y;
}

static double u_math_frexp_impl(double x, int *exp)
{
    double ax;
    int e;

    if (!exp)
    {
        return x;
    }

    if (x == 0.0 || u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        *exp = 0;
        return x;
    }

    ax = u_math_abs(x);
    e = 0;
    while (ax >= 1.0)
    {
        ax *= 0.5;
        e++;
    }
    while (ax < 0.5)
    {
        ax *= 2.0;
        e--;
    }

    *exp = e;
    return u_math_copysign(ax, x);
}

static double u_math_ldexp_impl(double x, int exp)
{
    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x) || x == 0.0)
    {
        return x;
    }

    while (exp > 0)
    {
        x *= 2.0;
        exp--;
    }
    while (exp < 0)
    {
        x *= 0.5;
        exp++;
    }
    return x;
}

static double u_math_sqrt_impl(double x)
{
    double guess;
    double prev;
    int i;

    if (x < 0.0 || u_math_is_nan_raw(x))
    {
        return 0.0 / 0.0;
    }
    if (x == 0.0 || u_math_is_inf_raw(x))
    {
        return x;
    }

    guess = x > 1.0 ? x : 1.0;
    prev = 0.0;
    for (i = 0; i < 20 && guess != prev; i++)
    {
        prev = guess;
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

static double u_math_exp_impl(double x)
{
    double term;
    double sum;
    double frac;
    int n;
    int i;

    if (u_math_is_nan_raw(x))
    {
        return x;
    }
    if (x > 709.0)
    {
        return 1.0 / 0.0;
    }
    if (x < -745.0)
    {
        return 0.0;
    }

    /*
     * 先把输入压到一个较小区间，再用泰勒级数。
     * 这不是数值库级别的实现，但足够支撑 CPython 的常见路径。
     */
    n = (int)u_math_floor(x);
    frac = x - (double)n;
    sum = 1.0;
    term = 1.0;
    for (i = 1; i < 18; i++)
    {
        term *= frac / (double)i;
        sum += term;
    }

    if (n > 0)
    {
        while (n-- > 0)
        {
            sum *= 2.71828182845904523536;
        }
    }
    else if (n < 0)
    {
        while (n++ < 0)
        {
            sum /= 2.71828182845904523536;
        }
    }
    return sum;
}

static double u_math_log_impl(double x)
{
    double m;
    double y;
    double y2;
    double sum;
    int e;
    int i;

    if (x < 0.0 || u_math_is_nan_raw(x))
    {
        return 0.0 / 0.0;
    }
    if (x == 0.0)
    {
        return -1.0 / 0.0;
    }
    if (u_math_is_inf_raw(x))
    {
        return 1.0 / 0.0;
    }

    m = u_math_frexp_impl(x, &e);
    /*
     * 让 mantissa 靠近 1，可以让 log 的级数收敛更快。
     */
    y = (m - 1.0) / (m + 1.0);
    y2 = y * y;
    sum = 0.0;
    for (i = 0; i < 19; i++)
    {
        sum += 2.0 * y / (2.0 * (double)i + 1.0);
        y *= y2;
    }

    return sum + (double)e * 0.69314718055994530942;
}

static double u_math_sin_impl(double x)
{
    double a;
    double xx;
    double term;
    double sum;
    int i;

    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        return 0.0 / 0.0;
    }

    a = u_math_fmod(x, 6.28318530717958647692);
    if (a > 3.14159265358979323846)
    {
        a -= 6.28318530717958647692;
    }
    else if (a < -3.14159265358979323846)
    {
        a += 6.28318530717958647692;
    }

    xx = a * a;
    term = a;
    sum = a;
    for (i = 1; i < 8; i++)
    {
        term *= -xx / ((2.0 * (double)i) * (2.0 * (double)i + 1.0));
        sum += term;
    }
    return sum;
}

static double u_math_cos_impl(double x)
{
    double a;
    double xx;
    double term;
    double sum;
    int i;

    if (u_math_is_nan_raw(x) || u_math_is_inf_raw(x))
    {
        return 0.0 / 0.0;
    }

    a = u_math_fmod(x, 6.28318530717958647692);
    if (a > 3.14159265358979323846)
    {
        a -= 6.28318530717958647692;
    }
    else if (a < -3.14159265358979323846)
    {
        a += 6.28318530717958647692;
    }

    xx = a * a;
    term = 1.0;
    sum = 1.0;
    for (i = 1; i < 8; i++)
    {
        term *= -xx / ((2.0 * (double)i - 1.0) * (2.0 * (double)i));
        sum += term;
    }
    return sum;
}

static double u_math_atan_impl(double x)
{
    double ax;
    double term;
    double sum;
    double xx;
    int i;

    if (u_math_is_nan_raw(x))
    {
        return x;
    }

    ax = u_math_abs(x);
    if (ax > 1.0)
    {
        double t;

        t = u_math_atan_impl(1.0 / x);
        if (x > 0.0)
        {
            return 1.57079632679489661923 - t;
        }
        return -1.57079632679489661923 - t;
    }

    xx = x * x;
    term = x;
    sum = x;
    for (i = 1; i < 12; i++)
    {
        term *= -xx;
        sum += term / (2.0 * (double)i + 1.0);
    }
    return sum;
}

static double u_math_atan2_impl(double y, double x)
{
    if (x > 0.0)
    {
        return u_math_atan_impl(y / x);
    }
    if (x < 0.0 && y >= 0.0)
    {
        return u_math_atan_impl(y / x) + 3.14159265358979323846;
    }
    if (x < 0.0 && y < 0.0)
    {
        return u_math_atan_impl(y / x) - 3.14159265358979323846;
    }
    if (x == 0.0 && y > 0.0)
    {
        return 1.57079632679489661923;
    }
    if (x == 0.0 && y < 0.0)
    {
        return -1.57079632679489661923;
    }
    return 0.0;
}

static double u_math_pow_impl(double x, double y)
{
    long long iy;
    double result;

    if (u_math_is_nan_raw(x) || u_math_is_nan_raw(y))
    {
        return 0.0 / 0.0;
    }
    if (y == 0.0)
    {
        return 1.0;
    }
    if (x == 0.0)
    {
        return y > 0.0 ? 0.0 : 1.0 / 0.0;
    }

    iy = (long long)y;
    if ((double)iy == y && u_math_abs(y) < 1024.0)
    {
        long long n;

        n = iy;
        result = 1.0;
        if (n < 0)
        {
            n = -n;
            while (n-- > 0)
            {
                result *= x;
            }
            return 1.0 / result;
        }

        while (n-- > 0)
        {
            result *= x;
        }
        return result;
    }

    if (x < 0.0)
    {
        return 0.0 / 0.0;
    }
    return u_math_exp_impl(y * u_math_log_impl(x));
}

static double u_math_hypot_impl(double x, double y)
{
    double ax;
    double ay;
    double m;
    double n;

    ax = u_math_abs(x);
    ay = u_math_abs(y);
    if (ax < ay)
    {
        m = ay;
        n = ax;
    }
    else
    {
        m = ax;
        n = ay;
    }
    if (m == 0.0)
    {
        return 0.0;
    }
    n /= m;
    return m * u_math_sqrt_impl(1.0 + n * n);
}

double fabs(double x)
{
    return u_math_abs(x);
}

double copysign(double x, double y)
{
    return u_math_copysign(x, y);
}

double floor(double x)
{
    return u_math_floor(x);
}

double ceil(double x)
{
    return u_math_ceil(x);
}

double trunc(double x)
{
    return u_math_trunc(x);
}

double round(double x)
{
    return u_math_round_nearest(x);
}

double fmod(double x, double y)
{
    return u_math_fmod(x, y);
}

double frexp(double x, int *exp)
{
    return u_math_frexp_impl(x, exp);
}

double ldexp(double x, int exp)
{
    return u_math_ldexp_impl(x, exp);
}

double sqrt(double x)
{
    return u_math_sqrt_impl(x);
}

double exp(double x)
{
    return u_math_exp_impl(x);
}

double log(double x)
{
    return u_math_log_impl(x);
}

double sin(double x)
{
    return u_math_sin_impl(x);
}

double cos(double x)
{
    return u_math_cos_impl(x);
}

double tan(double x)
{
    double c;

    c = cos(x);
    if (c == 0.0)
    {
        return x >= 0.0 ? 1.0 / 0.0 : -1.0 / 0.0;
    }
    return sin(x) / c;
}

double atan(double x)
{
    return u_math_atan_impl(x);
}

double atan2(double y, double x)
{
    return u_math_atan2_impl(y, x);
}

double pow(double x, double y)
{
    return u_math_pow_impl(x, y);
}

double hypot(double x, double y)
{
    return u_math_hypot_impl(x, y);
}

double modf(double x, double *iptr)
{
    double i;

    i = u_math_trunc(x);
    if (iptr)
    {
        *iptr = i;
    }
    return x - i;
}

int isnan(double x)
{
    return u_math_is_nan_raw(x) ? 1 : 0;
}

int isinf(double x)
{
    return u_math_is_inf_raw(x) ? (x < 0.0 ? -1 : 1) : 0;
}

int isfinite(double x)
{
    return (!u_math_is_nan_raw(x) && !u_math_is_inf_raw(x)) ? 1 : 0;
}

int finite(double x)
{
    return isfinite(x);
}

int signbit(double x)
{
    u_math_bits_t b;

    b.d = x;
    return (b.u & U_MATH_SIGN_MASK) != 0;
}
