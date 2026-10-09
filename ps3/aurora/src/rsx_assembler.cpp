#include "rsx_assembler.hpp"

#include <list>
#include <sstream>
#include <stack>
#include <string>

// cgcomp's headers are not self-contained; this is the order its own sources use.
#include "types.h"
#include "parser.h"
#include "fpparser.h"
#include "vpparser.h"
#include "compilerfp.h"
#include "compilervp.h"

namespace aurora::rsx {
namespace {
std::vector<char> to_buffer(const std::string& source) {
  std::vector<char> text(source.begin(), source.end());
  text.push_back('\0');
  return text;
}
} // namespace

AssembledProgram assemble_fragment_program(const std::string& source) {
  AssembledProgram out;
  auto text = to_buffer(source);
  CFPParser parser;
  CCompilerFP compiler;
  parser.Parse(text.data());
  out.parsedInstructions = static_cast<uint32_t>(parser.GetInstructionCount());
  compiler.Compile(&parser);
  out.instructionCount = static_cast<uint32_t>(compiler.GetInstructionCount());
  out.numRegs = static_cast<uint32_t>(compiler.GetNumRegs());
  out.fpControl = static_cast<uint32_t>(compiler.GetFPControl());
  out.texcoords = static_cast<uint32_t>(compiler.GetTexcoords());
  const auto* insns = compiler.GetInstructions();
  out.words.resize(static_cast<size_t>(out.instructionCount) * 4);
  for (uint32_t i = 0; i < out.instructionCount; ++i) {
    for (int j = 0; j < 4; ++j) {
      out.words[i * 4 + j] = insns[i].data[j];
    }
  }
  for (const auto& reloc : compiler.GetConstRelocations()) {
    out.constRelocations.emplace_back(reloc.offset, reloc.index);
  }
  return out;
}

AssembledProgram assemble_vertex_program(const std::string& source) {
  AssembledProgram out;
  auto text = to_buffer(source);
  CVPParser parser;
  CCompilerVP compiler;
  parser.Parse(text.data());
  compiler.Compile(&parser);
  out.instructionCount = static_cast<uint32_t>(compiler.GetInstructionCount());
  out.numRegs = static_cast<uint32_t>(compiler.GetNumRegs());
  out.inputMask = static_cast<uint32_t>(compiler.GetInputMask());
  out.outputMask = static_cast<uint32_t>(compiler.GetOutputMask());
  const auto* insns = compiler.GetInstructions();
  out.words.resize(static_cast<size_t>(out.instructionCount) * 4);
  for (uint32_t i = 0; i < out.instructionCount; ++i) {
    for (int j = 0; j < 4; ++j) {
      out.words[i * 4 + j] = insns[i].data[j];
    }
  }
  return out;
}
} // namespace aurora::rsx
