/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "IRCode.h"
#include "RedexContext.h"
#include "RedexTest.h"
#include "Show.h"

#include "OptimizeResources.h"

namespace {
uint32_t find_const_value(IRCode* code, IRInstruction* use, uint16_t reg) {
  // Janky scan that certainly won't work with control flow. Shouldn't matter
  // for just the autogenerated <clinit> method.
  int64_t literal = -1;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn == use) {
      break;
    } else if (insn->opcode() == OPCODE_CONST && insn->dest() == reg) {
      literal = insn->get_literal();
    }
  }
  always_assert_log(literal > 0, "Did not find const");
  return literal;
}

void dump_code_verbose(const IRCode* code) {
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    std::cout << SHOW(mie) << std::endl;
    if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      auto data = insn->get_data();
      std::cout << "  " << SHOW(data) << std::endl;
    }
  }
}

DexClass* get_r_class(const DexClasses& classes, const char* name) {
  DexClass* r_class = nullptr;
  for (const auto& cls : classes) {
    if (strcmp(cls->c_str(), name) == 0) {
      r_class = cls;
    }
  }
  always_assert_log(r_class != nullptr, "Did not find class %s!", name);
  auto clinit = r_class->get_clinit();
  always_assert_log(clinit != nullptr, "%s should have a <clinit>", name);
  auto code = clinit->get_code();
  always_assert_log(code != nullptr, "%s should have <clinit> code", name);
  return r_class;
}
} // namespace

class OptimizeResourcesTest : public RedexTest {};

TEST_F(OptimizeResourcesTest, remapResourceClassArrays) {
  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);

  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(
      load_classes_from_dex(DexLocation::make_location("dex", dexfile)));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  // Outer class that is assumed to have been customized to store extra junk.
  auto base_r_class_name = "Lcom/redextest/R;";
  ResourceConfig global_resources_config;
  global_resources_config.customized_r_classes.emplace(base_r_class_name);
  DexClass* base_r_class = get_r_class(classes, base_r_class_name);

  std::cout << "BASELINE R <clinit>:" << std::endl;
  auto clinit = base_r_class->get_clinit();
  auto code = clinit->get_code();
  dump_code_verbose(code);

  // A typical styleable inner class, which has different conventions and is
  // indexed directly into. Deletion should instead insert zeros.
  auto styleable_class_name = "Lcom/redextest/R$styleable;";
  DexClass* styleable_class = get_r_class(classes, styleable_class_name);
  std::cout << std::endl << "BASELINE R$styleable <clinit>:" << std::endl;
  auto styleable_clinit = styleable_class->get_clinit();
  auto styleable_code = styleable_clinit->get_code();
  dump_code_verbose(styleable_code);

  std::map<uint32_t, uint32_t> old_to_remapped_ids;
  // Remap all 4 items in the first array.
  old_to_remapped_ids.emplace(0x7f010000, 0x7f010010);
  old_to_remapped_ids.emplace(0x7f010001, 0x7f010011);
  old_to_remapped_ids.emplace(0x7f010002, 0x7f010012);
  old_to_remapped_ids.emplace(0x7f010003, 0x7f010013);
  // Keep the first two items from the second array, and delete the last 2.
  old_to_remapped_ids.emplace(0x7f020000, 0x7f020000);
  old_to_remapped_ids.emplace(0x7f020001, 0x7f020001);
  // Keep the first item from the third array, delete the last.
  old_to_remapped_ids.emplace(0x7f030000, 0x7f030000);
  // For styleable, delete first and keep last
  old_to_remapped_ids.emplace(0x7f040001, 0x7f040001);

  OptimizeResourcesPass::remap_resource_class_arrays(
      stores, global_resources_config, old_to_remapped_ids);

  std::cout << std::endl << "MODIFIED R <clinit>:" << std::endl;
  dump_code_verbose(code);

  std::vector<uint32_t> expected_sizes = {4, 2, 1};

  size_t count = 0;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_NEW_ARRAY) {
      auto size_reg = insn->src(0);
      auto array_size = find_const_value(code, insn, size_reg);
      EXPECT_EQ(array_size, expected_sizes[count++]) << "Array size mismatch";
    }
  }

  std::cout << std::endl << "MODIFIED R$styleable <clinit>:" << std::endl;
  dump_code_verbose(styleable_code);
  // Despite deleting one item, size should still be 2
  for (const auto& mie : InstructionIterable(styleable_code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_NEW_ARRAY) {
      auto size_reg = insn->src(0);
      auto array_size = find_const_value(code, insn, size_reg);
      EXPECT_EQ(array_size, 2) << "Array size mismatch for R$styleable";
    } else if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      auto elements = get_fill_array_data_payload<uint32_t>(insn->get_data());
      EXPECT_EQ(elements.size(), 2) << "Incorrect array payload size";
      EXPECT_EQ(elements[0], 0) << "First element should be zeroed out";
      EXPECT_EQ(elements[1], 0x7f040001)
          << "Second element should remain intact";
    }
  }
}