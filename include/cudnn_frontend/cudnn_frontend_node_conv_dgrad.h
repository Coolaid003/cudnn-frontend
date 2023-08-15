#pragma once

#include "../cudnn_frontend_ConvDesc.h"
#include "../cudnn_frontend_Heuristics.h"
#include "../cudnn_frontend_Logging.h"

#include "cudnn_frontend_graph_helpers.h"
#include "cudnn_frontend_node_interface.h"

namespace cudnn_frontend::graph {

class DgradNode : public INode {
    Conv_dgrad_attributes options;

   public:
    DgradNode(Conv_dgrad_attributes&& options_, detail::Context const& context)
        : INode(context), options(std::move(options_)) {}

    Type
    getType() override final {
        return Type::DGRAD;
    }

    error_t
    infer_properties_node() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferrencing properties for dgrad node " << options.name << "..."
                    << std::endl;

        options.fill_from_context(context);

        // TODO: Only inferrencing from (X, DY) -> DW works today.
        auto DX = options.outputs.DX;
        auto W  = options.inputs.W;
        auto DY = options.inputs.DY;

        auto const w_tensor_dim  = W->get_dim();
        auto const dy_tensor_dim = DY->get_dim();
        auto dx_tensor_dim       = DX->get_dim();

        // Only infer dims and strides if user did not set them
        if (dx_tensor_dim.empty()) {
            dx_tensor_dim.resize(w_tensor_dim.size());
            auto const& padding  = options.get_padding();
            auto const& stride   = options.get_stride();
            auto const& dilation = options.get_dilation();
            // x NCHW
            // w KCRS
            // y NKPQ
            // N
            dx_tensor_dim[0] = dy_tensor_dim[0];
            // C
            dx_tensor_dim[1] = w_tensor_dim[1];
            // HW
            for (size_t dim = 2; dim < w_tensor_dim.size(); ++dim) {
                dx_tensor_dim[dim] = (dy_tensor_dim[dim] - 1) * stride[dim - 2] - 2 * padding[dim - 2] + 1 +
                                     dilation[dim - 2] * (w_tensor_dim[dim] - 1);
            }
            DX->set_dim(dx_tensor_dim);
        }
        if (DX->get_stride().empty()) {
            DX->set_stride(detail::generate_stride(DX->get_dim()));
        }

        return {error_code_t::OK, ""};
    }

    error_t
    assign_uids_node() override final {
        options.inputs.DY->set_uid(ICudnn::create_new_uid());
        options.inputs.W->set_uid(ICudnn::create_new_uid());
        options.outputs.DX->set_uid(ICudnn::create_new_uid());
        return {error_code_t::OK, ""};
    }

    error_t
    createTensors() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building DgradNode tensors " << options.name << "..." << std::endl;

        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.outputs.DX));
        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.W));
        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.DY));

        return {error_code_t::OK, ""};
    }

    error_t
    createOperations() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building DgradNode operations " << options.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            // dgrad descriptor
            int64_t const spatial_dim_count = options.get_padding().size();
            auto dgrad_descriptor           = cudnn_frontend::ConvDescBuilder()
                                        .setComputeType(options.get_compute_data_type())
                                        .setMathMode(CUDNN_CROSS_CORRELATION)
                                        .setSpatialDimCount(spatial_dim_count)
                                        .setSpatialStride(spatial_dim_count, options.get_stride().data())
                                        .setPrePadding(spatial_dim_count, options.get_padding().data())
                                        .setPostPadding(spatial_dim_count, options.get_padding().data())
                                        .setDilation(spatial_dim_count, options.get_dilation().data())
                                        .build();

            // Create the dgrad operation.
            auto dgrad_operation =
                cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_CONVOLUTION_BACKWARD_DATA_DESCRIPTOR)
                    .setdxDesc(*(tensors.at(options.outputs.DX->get_uid())))
                    .setwDesc(*(tensors.at(options.inputs.W->get_uid())))
                    .setdyDesc(*(tensors.at(options.inputs.DY->get_uid())))
                    .setcDesc(dgrad_descriptor)
                    .setAlpha(1.f)
                    .setBeta(0.f)
                    .build();

            // Push all real tensors as required for operation execution.
            auto const& tensors_involved_in_operation = {options.outputs.DX, options.inputs.W, options.inputs.DY};

            std::vector<uid_t> uids_in_operation;
            for (auto const& tensor : tensors_involved_in_operation) {
                if (tensor && tensor->get_is_virtual() == false) {
                    uids_in_operation.push_back(tensor->get_uid());
                }
            }

            operations.push_back({std::move(dgrad_operation), std::move(uids_in_operation)});

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