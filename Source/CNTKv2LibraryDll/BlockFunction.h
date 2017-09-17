//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "PrimitiveFunction.h"
#include "Utils.h"
#include "Variable.h"

namespace CNTK
{
    class BlockFunction final : public PrimitiveFunction
    {
    public:
        BlockFunction(FunctionPtr&& composite,
                      const std::vector<std::pair<Variable, Variable>>& argumentsMap, // [composite's Placeholder] -> actual input it should pretend to be
                      const std::wstring& blockOpName, Dictionary&& attributes,
                      const std::wstring& blockName = L"", const std::wstring& uid = GenerateUid(PrimitiveOpType::Block))
            : PrimitiveFunction(PrimitiveOpType::Block, DetermineInputs(composite, argumentsMap, blockName), std::move(attributes), blockName, uid),
            m_composite(composite), m_blockOpName(blockOpName)
        {
        }

        // special version for InvokeGraph(). Defined in AutoBatch.cpp for now.
        BlockFunction(const std::shared_ptr<CompositeFunction>& composite, const std::vector<Variable>& operands, bool isBasicBlock, const std::wstring& blockName = std::wstring());
        Variable OutputForDynamicInvocation();

        virtual const std::wstring& OpName() const override { return m_blockOpName; }

        const FunctionPtr& Composite() const { return m_composite; }

        // Mapping from each argument of the composite underlying the block to the corresponding Variable it is mapped to
        std::vector<std::pair<Variable, Variable>> CompositeArgumentsMap() const
        {
            std::vector<std::pair<Variable, Variable>> argumentsMap;
            auto arguments = m_composite->Arguments();
            for (auto argument : arguments)
            {
                //if (BlockFunctionPlaceholderMapping(argument) == Variable())
                //    LogicError("BlockFunction '%S' with OpName '%S' does not have a mapping for argument '%S'.", AsString().c_str(), OpName().c_str(), argument.AsString().c_str());

                argumentsMap.push_back({ argument, BlockFunctionPlaceholderMapping(argument) });
            }

            // Now sort the mapping by the order of occurence of the argument mapping in the block's inputs
            auto blockInputs = Inputs();
            std::unordered_map<Variable, size_t> inputIndices;
            for (size_t i = 0; i < blockInputs.size(); ++i)
                inputIndices.insert({ blockInputs[i], i });

            std::stable_sort(argumentsMap.begin(), argumentsMap.end(), [&inputIndices](const std::pair<Variable, Variable>& first, const std::pair<Variable, Variable>& second) {
                return inputIndices.at(first.second) < inputIndices.at(second.second);
            });

            return argumentsMap;
        }

        // Mapping from each output of the block to the corresponding  output of underlying composite
        std::unordered_map<Variable, Variable> CompositeOutputsMap() const
        {
            std::unordered_map<Variable, Variable> outputsMap;
            auto outputs = RawOutputs();
            for (auto output : outputs)
            {
                //if (BlockFunctionOutputMapping(output) == Variable())
                //    LogicError("BlockFunction '%S' with OpName '%S' does not have a mapping for output '%S'", AsString().c_str(), OpName().c_str(), output.AsString().c_str());

                outputsMap[output] = BlockFunctionOutputMapping(output);
            }

            return outputsMap;
        }

        // determine for a Placeholder in m_composite which actual value (in m_inputs) it should pretend to be
        // Will fail if no mapping has been set up.
        const Variable& BlockFunctionPlaceholderMapping(const /*Placeholder*/Variable& argument) const
        {
            if (!argument.IsPlaceholder())
                LogicError("GetPlaceholderMapping can only be used for Placeholders.");
            if (!m_compositeIsShared)
            {
                if (argument.m_dataFields->m_compositeArgumentIndex != SIZE_MAX)
                    LogicError("m_compositeArgumentIndex should not be used when !m_compositeIsShared");
                if (!argument.m_dataFields->m_blockFunctionVariableMapping.m_dataFields)
                    LogicError("BlockFunction '%S' with OpName '%S' does not have a mapping for argument '%S'.", AsString().c_str(), OpName().c_str(), argument.AsString().c_str());
                return argument.m_dataFields->m_blockFunctionVariableMapping;
            }
            else
            {
                if (!argument.m_dataFields->m_blockFunctionVariableMapping.m_dataFields)
                    LogicError("m_blockFunctionVariableMapping should be set up when !m_compositeIsShared");
                if (argument.m_dataFields->m_compositeArgumentIndex == SIZE_MAX)
                    LogicError("BlockFunction '%S' with OpName '%S' does not have a mapping for argument '%S'.", AsString().c_str(), OpName().c_str(), argument.AsString().c_str());
                if (argument.m_dataFields->m_compositeArgumentIndex >= m_inputs.size())
                    LogicError("m_compositeArgumentIndex out of bounds??");
                return m_inputs[argument.m_dataFields->m_compositeArgumentIndex];
            }
        }

        // determine for an Output in this->m_outputs which outputs of m_composite it should pretend to be
        // Will fail if no mapping has been set up.
        const Variable& BlockFunctionOutputMapping(const /*Output*/Variable& output) const
        {
            if (!output.IsOutput())
                LogicError("BlockFunctionOutputMapping: Must only be called on OutputVariables");
            if (BlockFunctionOutputMapping(output) == Variable())
                LogicError("BlockFunction '%S' with OpName '%S' does not have a mapping for output '%S'", AsString().c_str(), OpName().c_str(), output.AsString().c_str());
            return output.m_dataFields->m_blockFunctionVariableMapping;
        }

    protected:
        virtual void OnPlaceholdersReplaced(const std::unordered_map<Variable, Variable>& placeholderReplacements,
                                            std::unordered_set<Variable>& replacedPlaceholders) override
        {
            // Substitute any placeholder replacements in the arguments map
            auto arguments = m_composite->Arguments();
            std::unordered_map<Variable, Variable> blockCompositePlaceholderReplacements;
            for (auto argument : arguments)
            {
                if (replacedPlaceholders.find(BlockFunctionPlaceholderMapping(argument)) != replacedPlaceholders.end())
                {
                    auto replacement = placeholderReplacements.at(BlockFunctionPlaceholderMapping(argument));
                    if (IsArgument(replacement))
                        argument.m_dataFields->m_blockFunctionVariableMapping = replacement;
                    else
                        blockCompositePlaceholderReplacements.insert({ argument,  replacement });
                }
            }

            m_composite->ReplacePlaceholders(blockCompositePlaceholderReplacements);
        }

    private:
        /*static*/ std::vector<Variable> DetermineInputs(const FunctionPtr& composite, const std::vector<std::pair<Variable, Variable>>& argumentsMap, const std::wstring& blockName) const
        {
            // The m_inputs of a BlockFunction are...
            std::unordered_map<Variable, Variable> argumentsMappingAsMap; // [composite's Placeholder] -> actual input it should pretend to be
            for (auto argumentMapping : argumentsMap)
            {
                auto wasInserted = argumentsMappingAsMap.insert(argumentMapping).second;
                if (!wasInserted)
                    InvalidArgument("Multiple mappings provided for argument '%S' of the Block composite '%S'", argumentMapping.first.AsString().c_str(), composite->AsString().c_str());
            }

            std::vector<Variable> blockFunctionInputs;  // the return value of this function is built here
            auto compositeInputs = composite->Inputs(); // (this is an expensive operation for composites, including a full traversal and a copy+shared_ptr of the inputs array)
            std::vector<Variable> unmappedArguments;
            for (auto compositeInput : compositeInputs) // compositeInputs includes both Placeholders and enclosed Parameters/Constants
            {
                assert(!compositeInput.IsOutput());

                if (compositeInput.IsConstant() || compositeInput.IsParameter())
                    blockFunctionInputs.push_back(compositeInput);
                else
                {
                    if (!compositeInput.IsPlaceholder())
                    {
                        InvalidArgument("The composite implementing Block '%S' has an argument '%S' which is not a placeholder. "
                                        "All arguments of the composite underlying a Block must be placeholders",
                                        blockName.c_str(), compositeInput.AsString().c_str());
                    }

                    // Verify that a mapping was provided for each Placeholder in the composite
                    if (argumentsMappingAsMap.find(compositeInput) == argumentsMappingAsMap.end())
                        unmappedArguments.push_back(compositeInput);
                }
            }

            if (!unmappedArguments.empty())
            {
                InvalidArgument("%zu of the Placeholders '%S' of the underlying composite Function of Block '%S' have not been mapped when encapsulating the composite as a Block.",
                                unmappedArguments.size(), NamedListString(unmappedArguments).c_str(), blockName.c_str());
            }

            // We now append the mapped arguments of the composite to the block inputs in the order of the map
            // instead of the original order they appear in the composite itself
            for (auto argumentMapping : argumentsMap)
            {
                argumentMapping.first.m_dataFields->m_blockFunctionVariableMapping = argumentMapping.second; // composite Placeholder remembers its actual input
                blockFunctionInputs.push_back(argumentMapping.second);
            }

            return blockFunctionInputs; // this goes into m_inputs
        }

        void InferOutputs(std::vector<Variable>& outputs) override
        {
            // We determine the outputs by replacing the arguments of the composite with new placeholders with updated 
            // shape etc. information matching the corresponding mapped input
            auto currentArguments = m_composite->Arguments(); // (this is an expensive operation, requiring a full traversal and a full copy+shared_ptr of the inputs array)
            std::unordered_map<Variable, Variable> replacementMap;
            for (auto currentArgument : currentArguments) // note: it is ensured that currentArguments only includes Placeholders (no Inputs or Outputs)
            {
                auto currentArgumentMapping = BlockFunctionPlaceholderMapping(currentArgument); // this was remembered in the constructor
                auto newArgument = PlaceholderLike(currentArgumentMapping);
                newArgument.m_dataFields->m_blockFunctionVariableMapping = currentArgumentMapping;

                replacementMap.insert({ currentArgument, newArgument });
            }

            m_composite->ReplacePlaceholders(replacementMap);

            assert(outputs.empty());
            auto compositeOutputs = m_composite->RawOutputs();
            for (auto compositeOutput : compositeOutputs)
            {
                auto output = OutputVariable(compositeOutput.Shape(), compositeOutput.GetDataType(), compositeOutput.DynamicAxes(), compositeOutput.NeedsGradient(), Name());
                output.m_dataFields->m_blockFunctionVariableMapping = compositeOutput;

                outputs.push_back(output);
            }
        }

    private:
        FunctionPtr m_composite;
        std::wstring m_blockOpName;

        // Dynamite:
        // In BlockFunctions created via Invoke(), the composite is shared across multiple invocations.
        // Therefore we cannot use Placeholder::m_blockFunctionVariableMapping to store the redirect
        // to the actual argument to be used in place of the Placeholder.
        // Instead, we use Placeholder::m_compositeArgumentIndex. The following conceptual equivalence
        // should hold: plVar->m_blockFunctionVariableMapping === m_inputs[plVar->m_compositeArgumentIndex].
        // TODO: Can we switch BlockFunction to this at large?
        bool m_compositeIsShared = false; // true for Dynamite

        // Increasing s_serializationVersion every time we add more ops allows us to print 
        // a more meaningful message when trying to load a new model with a stale binary. 
        static const size_t s_serializationVersion = 1;
    };
}
