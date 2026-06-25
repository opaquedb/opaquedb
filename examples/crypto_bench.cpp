// examples/crypto_bench.cpp
// Microbenchmark of the SEAL primitives the bit-sliced PIR scan uses, at
// OpaqueDB's default FHE parameters (read from CryptoConfig: poly 16384, the
// 349-bit coeff modulus). It answers "how does a rotation or a square compare
// to a ct*ct multiply?" with numbers from this machine, which is how the
// matcher's parameters were chosen: minimize ct*ct multiplies (the dominant
// cost), spend rotations and plaintext multiplies freely. Build in release;
// Debug timings are meaningless.

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <vector>

#include "opaquedb/config/config.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"
#include "seal/seal.h"

namespace {

using opaquedb::config::CryptoConfig;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;

double BenchUs(const char *name, int iters, const std::function<void()> &op) {
  for (int i = 0; i < 10; ++i)
    op(); // warm up
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i)
    op();
  const auto t1 = std::chrono::steady_clock::now();
  const double us =
      static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()) /
      1000.0 / iters;
  std::cout << "  " << name << ": " << us << " us\n";
  return us;
}

} // namespace

int main() {
  CryptoConfig cfg; // documented defaults: poly 16384, 349-bit modulus
  auto ctx = CryptoContext::Create(cfg);
  if (!ctx.ok()) {
    std::cerr << ctx.status().message() << "\n";
    return 1;
  }
  // Generate Galois keys too, so we can time rotation.
  ClientKeyring kr = ClientKeyring::Generate(*ctx, /*with_galois=*/true);
  seal::Evaluator ev(ctx->seal());

  std::vector<std::uint64_t> va(ctx->slot_count(), 2);
  std::vector<std::uint64_t> vb(ctx->slot_count(), 3);
  auto pa = ctx->EncodeBatch(va);
  auto pb = ctx->EncodeBatch(vb);
  auto a = opaquedb::crypto::Encrypt(*ctx, kr.public_key(), *pa);
  auto b = opaquedb::crypto::Encrypt(*ctx, kr.public_key(), *pb);
  if (!a.ok() || !b.ok()) {
    std::cerr << "encrypt failed\n";
    return 1;
  }
  const seal::Ciphertext &A = *a;
  const seal::Ciphertext &B = *b;
  seal::Ciphertext prod; // a size-3 ciphertext to relinearize
  ev.multiply(A, B, prod);

  const seal::RelinKeys &relin = kr.relin_keys();
  const seal::GaloisKeys &galois = kr.galois_keys();
  const int K = 500;

  std::cout << "SEAL primitives at poly " << cfg.poly_modulus_degree
            << " (release):\n";
  BenchUs("add (ct+ct)", K, [&] {
    seal::Ciphertext o;
    ev.add(A, B, o);
  });
  BenchUs("multiply_plain", K, [&] {
    seal::Ciphertext o;
    ev.multiply_plain(A, *pb, o);
  });
  BenchUs("square (ct^2, no relin)", K, [&] {
    seal::Ciphertext o = A;
    ev.square_inplace(o);
  });
  BenchUs("multiply (ct*ct, no relin)", K, [&] {
    seal::Ciphertext o;
    ev.multiply(A, B, o);
  });
  BenchUs("relinearize", K, [&] {
    seal::Ciphertext o = prod;
    ev.relinearize_inplace(o, relin);
  });
  BenchUs("multiply + relinearize", K, [&] {
    seal::Ciphertext o;
    ev.multiply(A, B, o);
    ev.relinearize_inplace(o, relin);
  });
  BenchUs("rotate_rows (Galois key-switch)", K, [&] {
    seal::Ciphertext o;
    ev.rotate_rows(A, 1, galois, o);
  });
  return 0;
}
