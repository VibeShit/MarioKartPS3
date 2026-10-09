// Runtime NV40 program assembly through the vendored PSL1GHT cgcomp assembler.
// Kept free of PSL1GHT and aurora headers: cgcomp declares its own copies of their types.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aurora::rsx {
struct AssembledProgram {
  // Instruction words exactly as cgcomp produced them (four per instruction).
  std::vector<uint32_t> words;
  // (instruction index holding the constant, constant register) for every c[] declared with #var.
  std::vector<std::pair<uint32_t, int>> constRelocations;
  uint32_t instructionCount = 0;
  uint32_t numRegs = 0;
  uint32_t fpControl = 0;
  uint32_t texcoords = 0;
  uint32_t inputMask = 0;
  uint32_t outputMask = 0;
};

AssembledProgram assemble_fragment_program(const std::string& source);
AssembledProgram assemble_vertex_program(const std::string& source);
} // namespace aurora::rsx
