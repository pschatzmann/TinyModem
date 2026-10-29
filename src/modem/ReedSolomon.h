#pragma once

#include <stdint.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Reed-Solomon code over GF(256) with the polynomial 0x11d and lengths
 * which are defined at runtime. The generator roots are alpha^fcr to
 * alpha^(fcr+ecc-1): fcr = 0 is used by ggwave (Mike Lubinets' library) and
 * IL2P, fcr = 1 by FX.25. The parity bytes follow the message, the first byte
 * is the highest power (like Phil Karn's library).
 * @copyright GPLv3
 */
class RSCodec {
 public:
  /// max length of message + parity
  static constexpr int MAX_LEN = 255;
  /// max number of parity bytes (FX.25: 64, ggwave: 56): limits the stack
  /// usage
  static constexpr int MAX_ECC = 64;

  /// Writes the message followed by ecc parity bytes (at most MAX_ECC, len +
  /// ecc <= MAX_LEN) to out
  static void encode(const uint8_t* msg, int len, int ecc, uint8_t* out,
                     int fcr = 0) {
    if (ecc < 1 || ecc > MAX_ECC || len + ecc > MAX_LEN) {
      LOGE("RSCodec: invalid length %d + %d", len, ecc);
      return;
    }
    uint8_t gen[MAX_ECC + 1];
    generator(ecc, gen, fcr);
    uint8_t work[MAX_LEN];
    memcpy(work, msg, len);
    memset(work + len, 0, ecc);
    // polynomial division: the remainder are the parity bytes
    for (int i = 0; i < len; i++) {
      uint8_t coef = work[i];
      if (coef == 0) continue;
      for (int j = 1; j <= ecc; j++) work[i + j] ^= mul(gen[j], coef);
    }
    memcpy(out, msg, len);
    memcpy(out + len, work + len, ecc);
  }

  /// Corrects up to ecc / 2 wrong bytes of the len + ecc bytes in place:
  /// returns false if the data can't be corrected
  static bool decode(uint8_t* data, int len, int ecc, int fcr = 0) {
    return correct(data, len, ecc, fcr) >= 0;
  }

  /// Corrects up to ecc / 2 wrong bytes of the len + ecc bytes in place:
  /// returns the number of corrected bytes or -1 if the data can't be
  /// corrected
  static int correct(uint8_t* data, int len, int ecc, int fcr = 0) {
    int n = len + ecc;
    if (n > MAX_LEN || ecc < 1 || ecc > MAX_ECC) return -1;
    uint8_t synd[MAX_ECC];
    if (!syndromes(data, n, ecc, synd, fcr)) return 0;  // no errors

    // Berlekamp-Massey: error locator lambda (ascending powers)
    uint8_t lambda[MAX_ECC + 2] = {1}, prev[MAX_ECC + 2] = {1};
    uint8_t tmp[MAX_ECC + 2];
    int nlambda = 1, nprev = 1, L = 0, m = 1;
    uint8_t b = 1;
    for (int r = 0; r < ecc; r++) {
      uint8_t d = synd[r];
      for (int i = 1; i <= L && i < nlambda; i++)
        d ^= mul(lambda[i], synd[r - i]);
      if (d == 0) {
        m++;
        continue;
      }
      uint8_t coef = div(d, b);
      bool grow = 2 * L <= r;
      if (grow) {
        memcpy(tmp, lambda, nlambda);
      }
      int ntmp = nlambda;
      // lambda -= coef * x^m * prev
      if (nprev + m > nlambda) {
        memset(lambda + nlambda, 0, nprev + m - nlambda);
        nlambda = nprev + m;
      }
      for (int i = 0; i < nprev; i++) lambda[i + m] ^= mul(coef, prev[i]);
      if (grow) {
        L = r + 1 - L;
        memcpy(prev, tmp, ntmp);
        nprev = ntmp;
        b = d;
        m = 1;
      } else {
        m++;
      }
    }
    if (2 * L > ecc) return -1;

    // Chien search: positions of the errors
    uint8_t pos[MAX_ECC / 2 + 1];
    int nerr = 0;
    for (int i = 0; i < n; i++) {
      int power = n - 1 - i;
      uint8_t x_inv = pow2(255 - power);  // alpha^-power
      if (eval(lambda, nlambda, x_inv) == 0) {
        if (nerr == L) return -1;
        pos[nerr++] = i;
      }
    }
    if (nerr != L) return -1;

    // Forney: omega = synd * lambda mod x^ecc
    uint8_t omega[MAX_ECC];
    for (int i = 0; i < ecc; i++) {
      uint8_t v = 0;
      for (int j = 0; j <= i && j < nlambda; j++)
        v ^= mul(lambda[j], synd[i - j]);
      omega[i] = v;
    }
    for (int k = 0; k < nerr; k++) {
      int power = n - 1 - pos[k];
      uint8_t x = pow2(power);
      uint8_t x_inv = pow2(255 - power);
      // derivative of lambda: odd coefficients
      uint8_t deriv = 0;
      for (int i = 1; i < nlambda; i += 2)
        deriv ^= mul(lambda[i], powOf(x_inv, i - 1));
      if (deriv == 0) return -1;
      // Forney: e = X^(1 - fcr) * omega(X^-1) / lambda'(X^-1)
      uint8_t factor = pow2(((1 - fcr) * power % 255 + 255) % 255);
      uint8_t e = mul(factor, div(eval(omega, ecc, x_inv), deriv));
      data[pos[k]] ^= e;
    }
    // the corrected data must be a code word
    return syndromes(data, n, ecc, synd, fcr) ? -1 : nerr;
  }

 protected:
  struct Tables {
    uint8_t exp[512];
    uint8_t log[256];
    Tables() {
      int x = 1;
      for (int i = 0; i < 255; i++) {
        exp[i] = x;
        log[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
      }
      for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
      log[0] = 0;
    }
  };

  static const Tables& tables() {
    static Tables result;
    return result;
  }

  static uint8_t mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    const Tables& t = tables();
    return t.exp[t.log[a] + t.log[b]];
  }

  static uint8_t div(uint8_t a, uint8_t b) {
    if (a == 0) return 0;
    const Tables& t = tables();
    return t.exp[(t.log[a] + 255 - t.log[b]) % 255];
  }

  /// alpha^power (power 0 - 255)
  static uint8_t pow2(int power) { return tables().exp[power % 255]; }

  static uint8_t powOf(uint8_t x, int power) {
    if (power == 0) return 1;
    if (x == 0) return 0;
    const Tables& t = tables();
    return t.exp[(t.log[x] * power) % 255];
  }

  /// Evaluates a polynomial with ascending powers
  static uint8_t eval(const uint8_t* poly, int n, uint8_t x) {
    uint8_t y = 0;
    for (int i = n - 1; i >= 0; i--) y = mul(y, x) ^ poly[i];
    return y;
  }

  /// Generator polynomial (descending powers) with ecc + 1 coefficients
  static void generator(int ecc, uint8_t* gen, int fcr) {
    gen[0] = 1;
    int len = 1;
    for (int i = 0; i < ecc; i++) {
      // gen *= (x + alpha^(fcr + i))
      uint8_t root = pow2(fcr + i);
      gen[len] = 0;
      for (int j = len; j > 0; j--) gen[j] ^= mul(gen[j - 1], root);
      len++;
    }
  }

  /// Calculates the syndromes (data[0] is the highest power): returns true
  /// if there are errors
  static bool syndromes(const uint8_t* data, int n, int ecc, uint8_t* synd,
                        int fcr) {
    bool errors = false;
    for (int j = 0; j < ecc; j++) {
      uint8_t x = pow2(fcr + j);
      uint8_t y = 0;
      for (int i = 0; i < n; i++) y = mul(y, x) ^ data[i];
      synd[j] = y;
      if (y != 0) errors = true;
    }
    return errors;
  }
};

}  // namespace tinymodem
