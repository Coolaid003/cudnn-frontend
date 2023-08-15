#pragma once

#include "../cudnn_frontend_Rng.h"
#include "../cudnn_frontend_Logging.h"

#include "cudnn_frontend_graph_helpers.h"
#include "cudnn_frontend_node_interface.h"

namespace cudnn_frontend::graph {

class RngNode : public INode {
    Rng_attributes options;

   public:
    RngNode(Rng_attributes&& options_, detail::Context const& context) : INode(context), options(std::move(options_)) {}

    Type
    getType() override final {
        return Type::RNG;
    }

    error_t
    validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating RngNode " << options.name << "..." << std::endl;

        if (!(options.outputs.Y)) {
            auto status         = error_code_t::ATTRIBUTE_NOT_SET;
            std::string message = "[cudnn_frontend] ERROR: rng output not set.";
            return {status, message};
        }

        return {error_code_t::OK, ""};
    }

    error_t
    assign_uids_node() override final {
        if (options.inputs.Seed) options.inputs.Seed->set_uid(ICudnn::create_new_uid());
        if (options.inputs.Offset) options.inputs.Offset->set_uid(ICudnn::create_new_uid());
        options.outputs.Y->set_uid(ICudnn::create_new_uid());
        return {error_code_t::OK, ""};
    }

    error_t
    createTensors() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building RngNode tensors " << options.name << "..." << std::endl;

        if (options.inputs.Seed) CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.Seed));
        if (options.inputs.Offset) CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.Offset));
        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.outputs.Y));

        return {error_code_t::OK, ""};
    }

    error_t
    createOperations() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building RngNode operations " << options.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            // Push all real tensors as required for operation execution.
            auto const& tensors_involved_in_operation = {options.inputs.Seed, options.inputs.Offset, options.outputs.Y};

            if (options.get_distribution() == RngDistribution_t::BERNOULLI) {
                auto rng_descriptor = cudnn_frontend::RngDescBuilder()
                                          .setRngDistribution(options.get_distribution())
                                          .setBernoulliDistProbability(options.get_bernoulli_probability().value())
                                          .build();

                if (options.inputs.Seed) {
                    auto Rng_operation = cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_RNG_DESCRIPTOR)
                                             .setyDesc(*(tensors.at(options.outputs.Y->get_uid())))
                                             .setRngDesc(rng_descriptor)
                                             .setSeedDesc(*(tensors.at(options.inputs.Seed->get_uid())))
                                             .setOffsetDesc(*(tensors.at(options.inputs.Offset->get_uid())))
                                             .build();

                    std::vector<uid_t> uids_in_operation;
                    for (auto const& tensor : tensors_involved_in_operation) {
                        if (tensor && tensor->get_is_virtual() == false) {
                            uids_in_operation.push_back(tensor->get_uid());
                        }
                    }

                    operations.push_back({std::move(Rng_operation), std::move(uids_in_operation)});

                } else {
                    auto Rng_operation = cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_RNG_DESCRIPTOR)
                                             .setyDesc(*(tensors.at(options.outputs.Y->get_uid())))
                                             .setRngDesc(rng_descriptor)
                                             .setSeed(options.get_seed().value())
                                             .build();

                    std::vector<uid_t> uids_in_operation;
                    for (auto const& tensor : tensors_involved_in_operation) {
                        if (tensor && tensor->get_is_virtual() == false) {
                            uids_in_operation.push_back(tensor->get_uid());
                        }
                    }

                    operations.push_back({std::move(Rng_operation), std::move(uids_in_operation)});
                }
            }

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        } catch (cudnn_frontend::cudnnException& e) {
            throw cudnnException(e.what(), e.getCudnnStatus());
        }
#endif

        return {error_code_t::OK, ""};
    }

    virtual void
    serialize(json& j) const override final {
        j = options;
    }
};

}  // namespace cudnn_frontend::graph