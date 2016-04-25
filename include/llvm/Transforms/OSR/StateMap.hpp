#ifndef STATEMAP_H
#define STATEMAP_H

#include <map>
#include <utility>

// State maps relate the instructions and resulting values in one function to
// instructions and values in anoither function. They allow us to translate
// locations and variables from one function to another.

struct StateMap {
	llvm::Function *F1, *F2;
	std::map<llvm::Value*, llvm::Value*> OneToOne;
	std::map<llvm::Instruction*, llvm::Instruction*> LandingPadMap;

	StateMap(llvm::Function *F1, llvm::Function *F2,
		 llvm::ValueToValueMapTy *VMap, bool bidirectional)
		: F1(F1), F2(F2)
	{
		for (llvm::ValueToValueMapTy::const_iterator it = VMap->begin(),
			     end = VMap->end();
		     it != end;
		     ++it) {
			llvm::Value *src = const_cast<llvm::Value *>(it->first);
			llvm::Value *dst = it->second;
			if (llvm::isa<llvm::Argument>(src)) {
				registerOneToOneValue(src, dst, bidirectional);
			} else if (auto src_inst =
				   llvm::dyn_cast<llvm::Instruction>(src)) {
				registerOneToOneValue(src, dst, bidirectional);
				if (!llvm::isa<llvm::PHINode>(src_inst)) {
					auto dst_inst =
						llvm::cast<llvm::Instruction>(
							dst);
					LandingPadMap[src_inst] = dst_inst;
					if (bidirectional) {
						LandingPadMap[dst_inst]
							= src_inst;
					}
				}
			}
		}
	}

	std::pair<llvm::BasicBlock*, llvm::ValueToValueMapTy*>
	genContinuationFunctionEntry(llvm::LLVMContext &context,
				     llvm::Instruction *OSR_src,
				     llvm::Instruction *landing,
				     llvm::Instruction *cont_landing,
				     std::vector<llvm::Value*> &values_to_set,
				     llvm::ValueToValueMapTy &fetched_values)
	{
		auto *updated_values = new llvm::ValueToValueMapTy();
		auto *entry_point = llvm::BasicBlock::Create(context, "osr_entry");
		for (auto *dst : values_to_set) {
			auto it = OneToOne.find(dst);
			if (it == OneToOne.end())
				llvm::report_fatal_error(
					"[genContinuationFunctionEntry]"
					" missing mapping information");
			auto *src = it->second;
			(*updated_values)[dst] = fetched_values[src];
		}
		auto *branch = llvm::BranchInst::Create(cont_landing->getParent());
		entry_point->getInstList().push_back(branch);
		return std::pair<llvm::BasicBlock*, llvm::ValueToValueMapTy*>(
			entry_point, updated_values);

	}

private:
	void registerOneToOneValue(llvm::Value *src, llvm::Value *dst,
				   bool bidirectional)
	{
		OneToOne[src] = dst;
		if (bidirectional) {
			OneToOne[dst] = src;
		}
	}
};

#endif
