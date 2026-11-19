/**
 * @file        estimates.h
 * @brief       The Xbox 360's estimate instructions, bit for bit (sdk/src/core/ppc_estimates.cpp)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * renut-ultimate (2026-11-16, port 144; tw2de_nx's, 2026-11-01): tables read from a retail console by Xenia (xenia-canary pull request #1342, BSD). The
 * vector ones take and return float bits and treat denormal inputs as zero (VSCR[NJ], which titles run with).
 */

#pragma once

#include <cstdint>

namespace rex::ppc::hw_est {
uint32_t EstimateReciprocal(uint32_t value);     // vrefp, vrefp128
uint32_t EstimateLog2(uint32_t value);           // vlogefp, vlogefp128
uint32_t EstimatePow2(uint32_t value);           // vexptefp, vexptefp128
double EstimateReciprocalSingle(double value);   // fres
}  // namespace rex::ppc::hw_est
