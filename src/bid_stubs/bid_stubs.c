/*
 * Stub implementation of the Intel BID64 Decimal Floating Point library.
 *
 * The IB TWS API uses Intel's DFP library for its Decimal type.
 * These stubs implement the required functions using standard IEEE 754
 * double arithmetic, storing doubles inside the Decimal (ull) slots.
 *
 * This works correctly for the IB API because all Decimal values arrive
 * over the wire as decimal strings (e.g. "150.25") and are converted via
 * __bid64_from_string. We store those as plain doubles bit-cast into the
 * unsigned long long Decimal, so the round-trip
 *   string -> Decimal -> double
 * is lossless for the precision IB uses.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef unsigned long long Decimal;

/* bit-cast helpers */
static double bid64_as_double(Decimal d) {
    double v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

static Decimal double_as_bid64(double v) {
    Decimal d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

/* ---- Required extern "C" functions ------------------------------------ */

Decimal __bid64_from_string(char* s, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return double_as_bid64(s ? strtod(s, NULL) : 0.0);
}

void __bid64_to_string(char* out, Decimal d, unsigned int* flags) {
    (void)flags;
    if (out) snprintf(out, 64, "%.10g", bid64_as_double(d));
}

double __bid64_to_binary64(Decimal d, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return bid64_as_double(d);
}

Decimal __binary64_to_bid64(double v, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return double_as_bid64(v);
}

Decimal __bid64_add(Decimal a, Decimal b, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return double_as_bid64(bid64_as_double(a) + bid64_as_double(b));
}

Decimal __bid64_sub(Decimal a, Decimal b, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return double_as_bid64(bid64_as_double(a) - bid64_as_double(b));
}

Decimal __bid64_mul(Decimal a, Decimal b, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    return double_as_bid64(bid64_as_double(a) * bid64_as_double(b));
}

Decimal __bid64_div(Decimal a, Decimal b, unsigned int rnd, unsigned int* flags) {
    (void)rnd; (void)flags;
    double db = bid64_as_double(b);
    return double_as_bid64(db != 0.0 ? bid64_as_double(a) / db : 0.0);
}
