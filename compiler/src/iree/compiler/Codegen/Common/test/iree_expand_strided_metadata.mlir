// RUN: iree-opt %s --iree-codegen-expand-strided-metadata="allow-unresolved=true" --split-input-file | FileCheck %s

#map0 = affine_map<(d0, d1)[s0] -> (d0 * 128 + s0 + d1)>
func.func @resolve_subview_memref(%arg0: memref<384x128xf32>, %arg1 : index, %arg2 : index) -> (memref<f32>, index, index, index, index, index) {
    %0 = memref.subview %arg0[%arg1, %arg2] [64, 64] [1, 1] : memref<384x128xf32> to memref<64x64xf32, #map0>
    %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<64x64xf32, #map0> -> memref<f32>, index, index, index, index, index
    return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
  }
//     CHECK: #[[MAP:.+]] = affine_map<()[s0, s1] -> (s0 * 128 + s1)>
//     CHECK: func @resolve_subview_memref(
// CHECK-DAG:   %[[C64:.+]] = arith.constant 64 : index
// CHECK-DAG:   %[[C128:.+]] = arith.constant 128 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[BASE_BUFFER:.+]], %[[BASE_OFFSET:.+]], %[[BASE_SIZES:.+]]:2, %[[BASE_STRIDES:.+]]:2 = memref.extract_strided_metadata %arg0
//     CHECK:   %[[SUB_OFFSET:.+]] = affine.apply #[[MAP]]()[%arg1, %arg2]
//     CHECK:   return %[[BASE_BUFFER]], %[[SUB_OFFSET]], %[[C64]], %[[C64]], %[[C128]], %[[C1]]

// -----

#map0 = affine_map<(d0)[s0] -> (d0 * 128 + s0)>
func.func @resolve_subview_rankreducing_memref(%arg0: memref<384x128xf32>, %arg1 : index, %arg2 : index) -> (memref<f32>, index, index, index) {
  %0 = memref.subview %arg0[%arg1, %arg2] [64, 1] [1, 1] : memref<384x128xf32> to memref<64xf32, #map0>
  %base_buffer, %offset, %size, %stride = memref.extract_strided_metadata %0 : memref<64xf32, #map0> -> memref<f32>, index, index, index
  return %base_buffer, %offset, %size, %stride : memref<f32>, index, index, index
}
//     CHECK: #[[MAP:.+]] = affine_map<()[s0, s1] -> (s0 * 128 + s1)>
//     CHECK: func @resolve_subview_rankreducing_memref(
// CHECK-DAG:   %[[C64:.+]] = arith.constant 64 : index
// CHECK-DAG:   %[[C128:.+]] = arith.constant 128 : index
// CHECK-DAG:   %[[BASE_BUFFER:.+]], %[[BASE_OFFSET:.+]], %[[BASE_SIZES:.+]]:2, %[[BASE_STRIDES:.+]]:2 = memref.extract_strided_metadata %arg0
// CHECK-DAG:   %[[SUB_OFFSET:.+]] = affine.apply #[[MAP]]()[%arg1, %arg2]
//     CHECK:   return %[[BASE_BUFFER]], %[[SUB_OFFSET]], %[[C64]], %[[C128]]

// -----

// Check that we properly resolve subview with rankreducing when the dropped
// rank is not the last one.
// Orig strides: [%strides#0, %strides#1, %strides#2]
// Sub strides: [1, 1, 1]
// => New strides: [%strides#0, %strides#1, %strides#2]
// Final strides == filterOutReducedDim(new strides, 0) == [%strides#1 , %strides#2]
//
// Orig offset: %offset
// Sub offsets: [%arg1, %arg2, 0]
// => Final offset: %arg1 * %strides#0 + %arg2 * %strides#1 + 0 * %strides#2 + %offset
//
// Final sizes == filterOutReducedDim(subview sizes, 0) == [6, 3]
//
func.func @resolve_subview_rankreducing_not_at_the_end_memref(%arg0: memref<8x16x4xf32>, %arg1 : index, %arg2 : index) -> (memref<f32>, index, index, index, index, index) {

  %0 = memref.subview %arg0[%arg1, %arg2, 0] [1, 6, 3] [1, 1, 1] : memref<8x16x4xf32> to memref<6x3xf32, strided<[4,1], offset : ?>>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<6x3xf32, strided<[4,1], offset : ?>> -> memref<f32>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
}
//     CHECK: #[[MAP:.+]] = affine_map<()[s0, s1] -> (s0 * 64 + s1 * 4)>
//     CHECK: func @resolve_subview_rankreducing_not_at_the_end_memref(
// CHECK-DAG:   %[[C6:.+]] = arith.constant 6 : index
// CHECK-DAG:   %[[C3:.+]] = arith.constant 3 : index
// CHECK-DAG:   %[[C4:.+]] = arith.constant 4 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[BASE_BUFFER:.+]], %[[BASE_OFFSET:.+]], %[[BASE_SIZES:.+]]:3, %[[BASE_STRIDES:.+]]:3 = memref.extract_strided_metadata %arg0
//     CHECK:   %[[SUB_OFFSET:.+]] = affine.apply #[[MAP]]()[%arg1, %arg2]
//     CHECK:   return %[[BASE_BUFFER]], %[[SUB_OFFSET]], %[[C6]], %[[C3]], %[[C4]], %[[C1]]

// -----

#pipeline_layout = #hal.pipeline.layout<bindings = [
  #hal.pipeline.binding<storage_buffer>
]>
func.func @resolve_binding_subspan_zero_offset_memref() -> (memref<f32>, index, index, index, index, index) {
  %c0 = arith.constant 0 : index
  %0 = hal.interface.binding.subspan layout(#pipeline_layout) binding(0) alignment(64) offset(%c0) : memref<512x384xf32>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<512x384xf32> -> memref<f32>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
}
//     CHECK: func @resolve_binding_subspan_zero_offset_memref(
// CHECK-DAG:   %[[C512:.+]] = arith.constant 512 : index
// CHECK-DAG:   %[[C384:.+]] = arith.constant 384 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
//     CHECK:   %[[BINDING:.+]] = hal.interface.binding.subspan layout({{.+}}) binding(0) alignment(64) offset(%[[C0]]) : memref<196608xf32>
//     CHECK:   %[[BASE_PTR:.+]] = memref.reinterpret_cast %[[BINDING]] to offset: [0], sizes: [], strides: []
//     CHECK:   return %[[BASE_PTR]], %[[C0]], %[[C512]], %[[C384]], %[[C384]], %[[C1]]

// -----

#pipeline_layout = #hal.pipeline.layout<bindings = [
  #hal.pipeline.binding<storage_buffer>
]>
func.func @resolve_binding_subspan_offset_index_memref(%arg0 : index) -> (memref<index>, index, index, index, index, index) {
  %0 = hal.interface.binding.subspan layout(#pipeline_layout) binding(0) alignment(64) offset(%arg0) : memref<512x384xindex, strided<[384, 1], offset:?>>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<512x384xindex, strided<[384, 1], offset:?>> -> memref<index>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<index>, index, index, index, index, index
}
//     CHECK: #[[MAP0:.+]] = affine_map<()[s0, s1] -> (s0 floordiv s1)>
//     CHECK: #[[MAP1:.+]] = affine_map<()[s0, s1] -> (s0 floordiv s1 + 196608)>
//     CHECK: func @resolve_binding_subspan_offset_index_memref(
// CHECK-DAG:   %[[C512:.+]] = arith.constant 512 : index
// CHECK-DAG:   %[[C384:.+]] = arith.constant 384 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
//     CHECK:   %[[SIZEOF:.+]] = util.sizeof index
//     CHECK:   %[[OFFSET:.+]] = affine.apply #[[MAP0]]()[%arg0, %[[SIZEOF]]]
//     CHECK:   %[[SUBSPAN_SIZE:.+]] = affine.apply #[[MAP1]]()[%arg0, %[[SIZEOF]]]
//     CHECK:   %[[BINDING:.+]] = hal.interface.binding.subspan layout({{.+}}) binding(0) alignment(64) offset(%[[C0]]) : memref<?xindex>{%[[SUBSPAN_SIZE]]}
//     CHECK:   %[[BASE_PTR:.+]] = memref.reinterpret_cast %[[BINDING]] to offset: [0], sizes: [], strides: []
//     CHECK:   return %[[BASE_PTR]], %[[OFFSET]], %[[C512]], %[[C384]], %[[C384]], %[[C1]]

// -----

#pipeline_layout = #hal.pipeline.layout<bindings = [
  #hal.pipeline.binding<storage_buffer>
]>
func.func @resolve_binding_subspan_dyn_dims_memref(%arg0 : index, %arg1 : index) -> (memref<index>, index, index, index, index, index) {
  %c0 = arith.constant 0 : index
  %0 = hal.interface.binding.subspan layout(#pipeline_layout) binding(0) alignment(64) offset(%c0) : memref<?x?xindex>{%arg0, %arg1}
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<?x?xindex> -> memref<index>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<index>, index, index, index, index, index
}
//     CHECK: #[[MAP:.+]] = affine_map<()[s0, s1] -> (s0 * s1)>
//     CHECK: func @resolve_binding_subspan_dyn_dims_memref(
// CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[SIZE:.+]] = affine.apply #[[MAP]]()[%arg0, %arg1]
//     CHECK:   %[[BINDING:.+]] = hal.interface.binding.subspan layout({{.+}}) binding(0) alignment(64) offset(%[[C0]]) : memref<?xindex>{%[[SIZE]]}
//     CHECK:   %[[BASE_PTR:.+]] = memref.reinterpret_cast %[[BINDING]] to offset: [0], sizes: [], strides: []
//     CHECK:   return %[[BASE_PTR]], %[[C0]], %arg0, %arg1, %arg1, %[[C1]]

// -----

func.func @resolve_alloca_static_memref() -> (memref<f32>, index, index, index, index, index) {
  %0 = memref.alloca() : memref<512x384xf32>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<512x384xf32> -> memref<f32>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
}
// CHECK-LABEL: func @resolve_alloca_static_memref(
//   CHECK-DAG:   %[[C512:.+]] = arith.constant 512 : index
//   CHECK-DAG:   %[[C384:.+]] = arith.constant 384 : index
//   CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
//   CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
//   CHECK-DAG:   %[[ALLOCA:.+]] = memref.alloca()
//       CHECK:   %[[CAST:.+]] = memref.reinterpret_cast %[[ALLOCA]]
//       CHECK:   return %[[CAST]], %[[C0]], %[[C512]], %[[C384]], %[[C384]], %[[C1]]

// -----

func.func @resolve_alloca_dynamic_memref(%arg0 : index) -> (memref<f32>, index, index, index, index, index) {
  %0 = memref.alloca(%arg0) : memref<?x384xf32>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<?x384xf32> -> memref<f32>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
}
// CHECK-LABEL: func @resolve_alloca_dynamic_memref(
//   CHECK-DAG:   %[[C384:.+]] = arith.constant 384 : index
//   CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
//   CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
//   CHECK-DAG:   %[[ALLOCA:.+]] = memref.alloca(%arg0)
//       CHECK:   %[[CAST:.+]] = memref.reinterpret_cast %[[ALLOCA]] to offset: [0], sizes: [], strides: []
//       CHECK:   return %[[CAST]], %[[C0]], %arg0, %[[C384]], %[[C384]], %[[C1]]

// -----

memref.global "private" constant @__constant_2xi32 : memref<512x384xf32> = dense<0.0>

func.func @resolve_global_memref() -> (memref<f32>, index, index, index, index, index) {
  %0 = memref.get_global @__constant_2xi32 : memref<512x384xf32>
  %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %0 : memref<512x384xf32> -> memref<f32>, index, index, index, index, index
  return %base_buffer, %offset, %sizes#0, %sizes#1, %strides#0, %strides#1 : memref<f32>, index, index, index, index, index
}
//     CHECK: memref.global "private" constant @[[CONSTANT:.+]] :
//     CHECK: func @resolve_global_memref()
// CHECK-DAG:   %[[C512:.+]] = arith.constant 512 : index
// CHECK-DAG:   %[[C384:.+]] = arith.constant 384 : index
// CHECK-DAG:   %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:   %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:   %[[GET_GLOBAL:.+]] = memref.get_global @[[CONSTANT]]
//     CHECK:   %[[CAST:.+]] = memref.reinterpret_cast %[[GET_GLOBAL]] to offset: [0], sizes: [], strides: []
//     CHECK:   return %[[CAST]], %[[C0]], %[[C512]], %[[C384]], %[[C384]], %[[C1]]

// -----

// Tests for the part of the pass that converts iree_codegen to memref.

#pipeline_layout = #hal.pipeline.layout<constants = 1, bindings = [
  #hal.pipeline.binding<storage_buffer>
]>
func.func @external_func_entry_point() -> (memref<bf16>, index) {
  %0 = hal.interface.constant.load layout(#pipeline_layout) ordinal(0) : i32
  %1 = arith.index_castui %0 : i32 to index
  %2 = hal.interface.binding.subspan layout(#pipeline_layout) binding(0) alignment(64) offset(%1) flags(ReadOnly) : memref<1x8x768xbf16, strided<[6144, 768, 1], offset: ?>>
  %base_buffer, %offset, %sizes:3, %strides:3 = iree_codegen.extract_strided_metadata %2 : memref<1x8x768xbf16, strided<[6144, 768, 1], offset: ?>> -> memref<bf16>, index, index, index, index, index, index, index
  return %base_buffer, %offset : memref<bf16>, index
}

// CHECK-LABEL: func.func @external_func_entry_point()
//       CHECK:   %[[SUBSPAN:.+]] = hal.interface.binding.subspan
//       CHECK:   %{{.+}} = memref.reinterpret_cast %[[SUBSPAN]]
