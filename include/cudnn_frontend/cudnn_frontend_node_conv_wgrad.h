#pragma once

#include "../cudnn_frontend_ConvDesc.h"
#include "../cudnn_frontend_Heuristics.h"
#include "../cudnn_frontend_Logging.h"

#include "cudnn_frontend_graph_helpers.h"
#include "cudnn_frontend_node_interface.h"

namespace cudnn_frontend::graph {

class WgradNode : public INode {
    Conv_wgrad_attributes options;

   public:
    WgradNode(Conv_wgrad_attributes&& options_, detail::Context const& context)
        : INode(context), options(std::move(options_)) {}

    Type
    getType() override final {
        return Type::WGRAD;
    }

    error_t
    infer_properties_node() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferrencing properties for conv node " << options.name << "."
                    << std::endl;

        options.fill_from_context(context);

        // TODO: Only inferrencing from (X, DY) -> DW works today.
        auto X  = options.inputs.X;
        auto DW = options.outputs.DW;
        auto DY = options.inputs.DY;

        auto const x_tensor_dim  = X->get_dim();
        auto const dy_tensor_dim = DY->get_dim();
        auto dw_tensor_dim       = DW->get_dim();

        // Only infer dims and strides if user did not set them
        if (dw_tensor_dim.empty()) {
            dw_tensor_dim.resize(x_tensor_dim.size());
            auto const& padding  = options.get_padding();
            auto const& stride   = options.get_stride();
            auto const& dilation = options.get_dilation();
            // x NCHW
            // w KCRS
            // y NKPQ
            // K
            dw_tensor_dim[0] = dy_tensor_dim[1];
            // C
            dw_tensor_dim[1] = x_tensor_dim[1];
            // RS
            for (size_t dim = 2; dim < x_tensor_dim.size(); ++dim) {
                dw_tensor_dim[dim] =
                    (x_tensor_dim[dim] + 2 * padding[dim - 2] - (dy_tensor_dim[dim] - 1) * stride[dim - 2] - 1) /
                        dilation[dim - 2] +
                    1;
            }
            DW->set_dim(dw_tensor_dim);
        }
        if (DW->get_stride().empty()) {
            DW->set_stride(detail::generate_stride(DW->get_dim()));
        }

        return {error_code_t::OK, ""};
    }

    error_t
    assign_uids_node() override final {
        options.inputs.DY->set_uid(ICudnn::create_new_uid());
        options.inputs.X->set_uid(ICudnn::create_new_uid());
        options.outputs.DW->set_uid(ICudnn::create_new_uid());
        return {error_code_t::OK, ""};
    }

    error_t
    createTensors() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building WgradNode tensors " << options.name << "..." << std::endl;

        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.X));
        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.inputs.DY));
        CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(options.outputs.DW));

        return {error_code_t::OK, ""};
    }

    error_t
    createOperations() override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building WgradNode operations " << options.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            // wgrad descriptor
            int64_t const spatial_dim_count = options.get_padding().size();
            auto wgrad_descriptor           = cudnn_frontend::ConvDescBuilder()
                                        .setComputeType(options.get_compute_data_type())
                                        .setMathMode(CUDNN_CROSS_CORRELATION)
                                        .setSpatialDimCount(spatial_dim_count)
                                        .setSpatialStride(spatial_dim_count, options.get_stride().data())
                                        .setPrePadding(spatial_dim_count, options.get_padding().data())
                                        .setPostPadding(spatial_dim_count, options.get_padding().data())
                                        .setDilation(spatial_dim_count, options.get_dilation().data())
                                        .build();

            // Create the wgrad operation.
            auto wgrad_operation =
                cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_CONVOLUTION_BACKWARD_FILTER_DESCRIPTOR)
                    .setxDesc(*(tensors.at(options.inputs.X->get_uid())))
                    .setdyDesc(*(tensors.at(options.inputs.DY->get_uid())))
                    .setdwDesc(*(tensors.at(options.outputs.DW->get_uid())))
                    .setcDesc(wgrad_descriptor)
                    .setAlpha(1.f)
                    .setBeta(0.f)
                    .build();

            // Push all real tensors as required for operation execution.
            auto const& tensors_involved_in_operation = {options.inputs.X, options.inputs.DY, options.outputs.DW};

            std::vector<uid_t> uids_in_operation;
            for (auto const& tensor : tensors_involved_in_operation) {
                if (tensor && tensor->get_is_virtual() == false) {
                    uids_in_operation.push_back(tensor->get_uid());
                }
            }

            operations.push_back({std::move(wgrad_operation), std::move(uids_in_operation)});

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