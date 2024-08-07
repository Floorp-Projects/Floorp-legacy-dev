/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! A picture represents a dynamically rendered image.
//!
//! # Overview
//!
//! Pictures consists of:
//!
//! - A number of primitives that are drawn onto the picture.
//! - A composite operation describing how to composite this
//!   picture into its parent.
//! - A configuration describing how to draw the primitives on
//!   this picture (e.g. in screen space or local space).
//!
//! The tree of pictures are generated during scene building.
//!
//! Depending on their composite operations pictures can be rendered into
//! intermediate targets or folded into their parent picture.
//!
//! ## Picture caching
//!
//! Pictures can be cached to reduce the amount of rasterization happening per
//! frame.
//!
//! When picture caching is enabled, the scene is cut into a small number of slices,
//! typically:
//!
//! - content slice
//! - UI slice
//! - background UI slice which is hidden by the other two slices most of the time.
//!
//! Each of these slice is made up of fixed-size large tiles of 2048x512 pixels
//! (or 128x128 for the UI slice).
//!
//! Tiles can be either cached rasterized content into a texture or "clear tiles"
//! that contain only a solid color rectangle rendered directly during the composite
//! pass.
//!
//! ## Invalidation
//!
//! Each tile keeps track of the elements that affect it, which can be:
//!
//! - primitives
//! - clips
//! - image keys
//! - opacity bindings
//! - transforms
//!
//! These dependency lists are built each frame and compared to the previous frame to
//! see if the tile changed.
//!
//! The tile's primitive dependency information is organized in a quadtree, each node
//! storing an index buffer of tile primitive dependencies.
//!
//! The union of the invalidated leaves of each quadtree produces a per-tile dirty rect
//! which defines the scissor rect used when replaying the tile's drawing commands and
//! can be used for partial present.
//!
//! ## Display List shape
//!
//! WR will first look for an iframe item in the root stacking context to apply
//! picture caching to. If that's not found, it will apply to the entire root
//! stacking context of the display list. Apart from that, the format of the
//! display list is not important to picture caching. Each time a new scroll root
//! is encountered, a new picture cache slice will be created. If the display
//! list contains more than some arbitrary number of slices (currently 8), the
//! content will all be squashed into a single slice, in order to save GPU memory
//! and compositing performance.
//!
//! ## Compositor Surfaces
//!
//! Sometimes, a primitive would prefer to exist as a native compositor surface.
//! This allows a large and/or regularly changing primitive (such as a video, or
//! webgl canvas) to be updated each frame without invalidating the content of
//! tiles, and can provide a significant performance win and battery saving.
//!
//! Since drawing a primitive as a compositor surface alters the ordering of
//! primitives in a tile, we use 'overlay tiles' to ensure correctness. If a
//! tile has a compositor surface, _and_ that tile has primitives that overlap
//! the compositor surface rect, the tile switches to be drawn in alpha mode.
//!
//! We rely on only promoting compositor surfaces that are opaque primitives.
//! With this assumption, the tile(s) that intersect the compositor surface get
//! a 'cutout' in the rectangle where the compositor surface exists (not the
//! entire tile), allowing that tile to be drawn as an alpha tile after the
//! compositor surface.
//!
//! Tiles are only drawn in overlay mode if there is content that exists on top
//! of the compositor surface. Otherwise, we can draw the tiles in the normal fast
//! path before the compositor surface is drawn. Use of the per-tile valid and
//! dirty rects ensure that we do a minimal amount of per-pixel work here to
//! blend the overlay tile (this is not always optimal right now, but will be
//! improved as a follow up).

use api::{MixBlendMode, PremultipliedColorF, FilterPrimitiveKind};
use api::{PropertyBinding, PropertyBindingId, FilterPrimitive, RasterSpace};
use api::{DebugFlags, ImageKey, ColorF, ColorU, PrimitiveFlags};
use api::{ImageRendering, ColorDepth, YuvRangedColorSpace, YuvFormat, AlphaType};
use api::units::*;
use crate::command_buffer::PrimitiveCommand;
use crate::box_shadow::BLUR_SAMPLE_SCALE;
use crate::clip::{ClipStore, ClipChainInstance, ClipLeafId, ClipNodeId, ClipTreeBuilder};
use crate::spatial_tree::{SpatialTree, CoordinateSpaceMapping, SpatialNodeIndex, VisibleFace};
use crate::composite::{CompositorKind, CompositeState, NativeSurfaceId, NativeTileId, CompositeTileSurface, tile_kind};
use crate::composite::{ExternalSurfaceDescriptor, ExternalSurfaceDependency, CompositeTileDescriptor, CompositeTile};
use crate::composite::{CompositorTransformIndex};
use crate::debug_colors;
use euclid::{vec3, Point2D, Scale, Vector2D, Box2D};
use euclid::approxeq::ApproxEq;
use crate::filterdata::SFilterData;
use crate::intern::ItemUid;
use crate::internal_types::{FastHashMap, FastHashSet, PlaneSplitter, Filter, FrameId};
use crate::internal_types::{PlaneSplitterIndex, PlaneSplitAnchor, TextureSource};
use crate::frame_builder::{FrameBuildingContext, FrameBuildingState, PictureState, PictureContext};
use crate::gpu_cache::{GpuCache, GpuCacheAddress, GpuCacheHandle};
use crate::gpu_types::{UvRectKind, ZBufferId};
use peek_poke::{PeekPoke, poke_into_vec, peek_from_slice, ensure_red_zone};
use plane_split::{Clipper, Polygon};
use crate::prim_store::{PrimitiveTemplateKind, PictureIndex, PrimitiveInstance, PrimitiveInstanceKind};
use crate::prim_store::{ColorBindingStorage, ColorBindingIndex, PrimitiveScratchBuffer};
use crate::print_tree::{PrintTree, PrintTreePrinter};
use crate::render_backend::DataStores;
use crate::render_task_graph::RenderTaskId;
use crate::render_target::RenderTargetKind;
use crate::render_task::{BlurTask, RenderTask, RenderTaskLocation, BlurTaskCache};
use crate::render_task::{StaticRenderTaskSurface, RenderTaskKind};
use crate::renderer::BlendMode;
use crate::resource_cache::{ResourceCache, ImageGeneration, ImageRequest};
use crate::space::SpaceMapper;
use crate::scene::SceneProperties;
use crate::spatial_tree::CoordinateSystemId;
use crate::surface::{SurfaceDescriptor, SurfaceTileDescriptor};
use smallvec::SmallVec;
use std::{mem, u8, marker, u32};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::collections::hash_map::Entry;
use std::ops::Range;
use crate::picture_textures::PictureCacheTextureHandle;
use crate::util::{MaxRect, VecHelper, MatrixHelpers, Recycler, ScaleOffset};
use crate::filterdata::{FilterDataHandle};
use crate::tile_cache::{SliceDebugInfo, TileDebugInfo, DirtyTileDebugInfo};
use crate::visibility::{PrimitiveVisibilityFlags, FrameVisibilityContext};
use crate::visibility::{VisibilityState, FrameVisibilityState};
use crate::scene_building::{SliceFlags};

// Maximum blur radius for blur filter (different than box-shadow blur).
// Taken from FilterNodeSoftware.cpp in Gecko.
const MAX_BLUR_RADIUS: f32 = 100.;

/// Specify whether a surface allows subpixel AA text rendering.
#[derive(Debug, Copy, Clone)]
pub enum SubpixelMode {
    /// This surface allows subpixel AA text
    Allow,
    /// Subpixel AA text cannot be drawn on this surface
    Deny,
    /// Subpixel AA can be drawn on this surface, if not intersecting
    /// with the excluded regions, and inside the allowed rect.
    Conditional {
        allowed_rect: PictureRect,
    },
}

/// A comparable transform matrix, that compares with epsilon checks.
#[derive(Debug, Clone)]
struct MatrixKey {
    m: [f32; 16],
}

impl PartialEq for MatrixKey {
    fn eq(&self, other: &Self) -> bool {
        const EPSILON: f32 = 0.001;

        // TODO(gw): It's possible that we may need to adjust the epsilon
        //           to be tighter on most of the matrix, except the
        //           translation parts?
        for (i, j) in self.m.iter().zip(other.m.iter()) {
            if !i.approx_eq_eps(j, &EPSILON) {
                return false;
            }
        }

        true
    }
}

/// A comparable scale-offset, that compares with epsilon checks.
#[derive(Debug, Clone)]
struct ScaleOffsetKey {
    sx: f32,
    sy: f32,
    tx: f32,
    ty: f32,
}

impl PartialEq for ScaleOffsetKey {
    fn eq(&self, other: &Self) -> bool {
        const EPSILON: f32 = 0.001;

        self.sx.approx_eq_eps(&other.sx, &EPSILON) &&
        self.sy.approx_eq_eps(&other.sy, &EPSILON) &&
        self.tx.approx_eq_eps(&other.tx, &EPSILON) &&
        self.ty.approx_eq_eps(&other.ty, &EPSILON)
    }
}

/// A comparable / hashable version of a coordinate space mapping. Used to determine
/// if a transform dependency for a tile has changed.
#[derive(Debug, PartialEq, Clone)]
enum TransformKey {
    Local,
    ScaleOffset {
        so: ScaleOffsetKey,
    },
    Transform {
        m: MatrixKey,
    }
}

impl<Src, Dst> From<CoordinateSpaceMapping<Src, Dst>> for TransformKey {
    fn from(transform: CoordinateSpaceMapping<Src, Dst>) -> TransformKey {
        match transform {
            CoordinateSpaceMapping::Local => {
                TransformKey::Local
            }
            CoordinateSpaceMapping::ScaleOffset(ref scale_offset) => {
                TransformKey::ScaleOffset {
                    so: ScaleOffsetKey {
                        sx: scale_offset.scale.x,
                        sy: scale_offset.scale.y,
                        tx: scale_offset.offset.x,
                        ty: scale_offset.offset.y,
                    }
                }
            }
            CoordinateSpaceMapping::Transform(ref m) => {
                TransformKey::Transform {
                    m: MatrixKey {
                        m: m.to_array(),
                    },
                }
            }
        }
    }
}

/// Unit for tile coordinates.
#[derive(Hash, Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct TileCoordinate;

// Geometry types for tile coordinates.
pub type TileOffset = Point2D<i32, TileCoordinate>;
pub type TileRect = Box2D<i32, TileCoordinate>;

/// The maximum number of compositor surfaces that are allowed per picture cache. This
/// is an arbitrary number that should be enough for common cases, but low enough to
/// prevent performance and memory usage drastically degrading in pathological cases.
pub const MAX_COMPOSITOR_SURFACES: usize = 4;

/// The size in device pixels of a normal cached tile.
pub const TILE_SIZE_DEFAULT: DeviceIntSize = DeviceIntSize {
    width: 1024,
    height: 512,
    _unit: marker::PhantomData,
};

/// The size in device pixels of a tile for horizontal scroll bars
pub const TILE_SIZE_SCROLLBAR_HORIZONTAL: DeviceIntSize = DeviceIntSize {
    width: 1024,
    height: 32,
    _unit: marker::PhantomData,
};

/// The size in device pixels of a tile for vertical scroll bars
pub const TILE_SIZE_SCROLLBAR_VERTICAL: DeviceIntSize = DeviceIntSize {
    width: 32,
    height: 1024,
    _unit: marker::PhantomData,
};

/// The maximum size per axis of a surface,
///  in WorldPixel coordinates.
const MAX_SURFACE_SIZE: usize = 4096;
/// Maximum size of a compositor surface.
const MAX_COMPOSITOR_SURFACES_SIZE: f32 = 8192.0;

/// Used to get unique tile IDs, even when the tile cache is
/// destroyed between display lists / scenes.
static NEXT_TILE_ID: AtomicUsize = AtomicUsize::new(0);

fn clamp(value: i32, low: i32, high: i32) -> i32 {
    value.max(low).min(high)
}

fn clampf(value: f32, low: f32, high: f32) -> f32 {
    value.max(low).min(high)
}

/// An index into the prims array in a TileDescriptor.
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct PrimitiveDependencyIndex(pub u32);

/// Information about the state of a binding.
#[derive(Debug)]
pub struct BindingInfo<T> {
    /// The current value retrieved from dynamic scene properties.
    value: T,
    /// True if it was changed (or is new) since the last frame build.
    changed: bool,
}

/// Information stored in a tile descriptor for a binding.
#[derive(Debug, PartialEq, Clone, Copy, PeekPoke)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum Binding<T> {
    Value(T),
    Binding(PropertyBindingId),
}

impl<T: Default> Default for Binding<T> {
    fn default() -> Self {
        Binding::Value(T::default())
    }
}

impl<T> From<PropertyBinding<T>> for Binding<T> {
    fn from(binding: PropertyBinding<T>) -> Binding<T> {
        match binding {
            PropertyBinding::Binding(key, _) => Binding::Binding(key.id),
            PropertyBinding::Value(value) => Binding::Value(value),
        }
    }
}

pub type OpacityBinding = Binding<f32>;
pub type OpacityBindingInfo = BindingInfo<f32>;

pub type ColorBinding = Binding<ColorU>;
pub type ColorBindingInfo = BindingInfo<ColorU>;

#[derive(PeekPoke)]
enum PrimitiveDependency {
    OpacityBinding {
        binding: OpacityBinding,
    },
    ColorBinding {
        binding: ColorBinding,
    },
    SpatialNode {
        index: SpatialNodeIndex,
    },
    Clip {
        clip: ItemUid,
    },
    Image {
        image: ImageDependency,
    },
}

/// A dependency for a transform is defined by the spatial node index + frame it was used
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash, PeekPoke, Default)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SpatialNodeKey {
    spatial_node_index: SpatialNodeIndex,
    frame_id: FrameId,
}

/// A helper for comparing spatial nodes between frames. The comparisons
/// are done by-value, so that if the shape of the spatial node tree
/// changes, invalidations aren't done simply due to the spatial node
/// index changing between display lists.
struct SpatialNodeComparer {
    /// The root spatial node index of the tile cache
    ref_spatial_node_index: SpatialNodeIndex,
    /// Maintains a map of currently active transform keys
    spatial_nodes: FastHashMap<SpatialNodeKey, TransformKey>,
    /// A cache of recent comparisons between prev and current spatial nodes
    compare_cache: FastHashMap<(SpatialNodeKey, SpatialNodeKey), bool>,
    /// A set of frames that we need to retain spatial node entries for
    referenced_frames: FastHashSet<FrameId>,
}

impl SpatialNodeComparer {
    /// Construct a new comparer
    fn new() -> Self {
        SpatialNodeComparer {
            ref_spatial_node_index: SpatialNodeIndex::INVALID,
            spatial_nodes: FastHashMap::default(),
            compare_cache: FastHashMap::default(),
            referenced_frames: FastHashSet::default(),
        }
    }

    /// Advance to the next frame
    fn next_frame(
        &mut self,
        ref_spatial_node_index: SpatialNodeIndex,
    ) {
        // Drop any node information for unreferenced frames, to ensure that the
        // hashmap doesn't grow indefinitely!
        let referenced_frames = &self.referenced_frames;
        self.spatial_nodes.retain(|key, _| {
            referenced_frames.contains(&key.frame_id)
        });

        // Update the root spatial node for this comparer
        self.ref_spatial_node_index = ref_spatial_node_index;
        self.compare_cache.clear();
        self.referenced_frames.clear();
    }

    /// Register a transform that is used, and build the transform key for it if new.
    fn register_used_transform(
        &mut self,
        spatial_node_index: SpatialNodeIndex,
        frame_id: FrameId,
        spatial_tree: &SpatialTree,
    ) {
        let key = SpatialNodeKey {
            spatial_node_index,
            frame_id,
        };

        if let Entry::Vacant(entry) = self.spatial_nodes.entry(key) {
            entry.insert(
                get_transform_key(
                    spatial_node_index,
                    self.ref_spatial_node_index,
                    spatial_tree,
                )
            );
        }
    }

    /// Return true if the transforms for two given spatial nodes are considered equivalent
    fn are_transforms_equivalent(
        &mut self,
        prev_spatial_node_key: &SpatialNodeKey,
        curr_spatial_node_key: &SpatialNodeKey,
    ) -> bool {
        let key = (*prev_spatial_node_key, *curr_spatial_node_key);
        let spatial_nodes = &self.spatial_nodes;

        *self.compare_cache
            .entry(key)
            .or_insert_with(|| {
                let prev = &spatial_nodes[&prev_spatial_node_key];
                let curr = &spatial_nodes[&curr_spatial_node_key];
                curr == prev
            })
    }

    /// Ensure that the comparer won't GC any nodes for a given frame id
    fn retain_for_frame(&mut self, frame_id: FrameId) {
        self.referenced_frames.insert(frame_id);
    }
}

// Immutable context passed to picture cache tiles during pre_update
struct TilePreUpdateContext {
    /// Maps from picture cache coords -> world space coords.
    pic_to_world_mapper: SpaceMapper<PicturePixel, WorldPixel>,

    /// The optional background color of the picture cache instance
    background_color: Option<ColorF>,

    /// The visible part of the screen in world coords.
    global_screen_world_rect: WorldRect,

    /// Current size of tiles in picture units.
    tile_size: PictureSize,

    /// The current frame id for this picture cache
    frame_id: FrameId,
}

// Immutable context passed to picture cache tiles during update_dirty_and_valid_rects
struct TileUpdateDirtyContext<'a> {
    /// Maps from picture cache coords -> world space coords.
    pic_to_world_mapper: SpaceMapper<PicturePixel, WorldPixel>,

    /// Global scale factor from world -> device pixels.
    global_device_pixel_scale: DevicePixelScale,

    /// Information about opacity bindings from the picture cache.
    opacity_bindings: &'a FastHashMap<PropertyBindingId, OpacityBindingInfo>,

    /// Information about color bindings from the picture cache.
    color_bindings: &'a FastHashMap<PropertyBindingId, ColorBindingInfo>,

    /// The local rect of the overall picture cache
    local_rect: PictureRect,

    /// If true, the scale factor of the root transform for this picture
    /// cache changed, so we need to invalidate the tile and re-render.
    invalidate_all: bool,
}

// Mutable state passed to picture cache tiles during update_dirty_and_valid_rects
struct TileUpdateDirtyState<'a> {
    /// Allow access to the texture cache for requesting tiles
    resource_cache: &'a mut ResourceCache,

    /// Current configuration and setup for compositing all the picture cache tiles in renderer.
    composite_state: &'a mut CompositeState,

    /// A cache of comparison results to avoid re-computation during invalidation.
    compare_cache: &'a mut FastHashMap<PrimitiveComparisonKey, PrimitiveCompareResult>,

    /// Information about transform node differences from last frame.
    spatial_node_comparer: &'a mut SpatialNodeComparer,
}

// Immutable context passed to picture cache tiles during post_update
struct TilePostUpdateContext {
    /// The local clip rect (in picture space) of the entire picture cache
    local_clip_rect: PictureRect,

    /// The calculated backdrop information for this cache instance.
    backdrop: Option<BackdropInfo>,

    /// Current size in device pixels of tiles for this cache
    current_tile_size: DeviceIntSize,

    /// Pre-allocated z-id to assign to tiles during post_update.
    z_id: ZBufferId,
}

// Mutable state passed to picture cache tiles during post_update
struct TilePostUpdateState<'a> {
    /// Allow access to the texture cache for requesting tiles
    resource_cache: &'a mut ResourceCache,

    /// Current configuration and setup for compositing all the picture cache tiles in renderer.
    composite_state: &'a mut CompositeState,
}

/// Information about the dependencies of a single primitive instance.
struct PrimitiveDependencyInfo {
    /// Unique content identifier of the primitive.
    prim_uid: ItemUid,

    /// The (conservative) clipped area in picture space this primitive occupies.
    prim_clip_box: PictureBox2D,

    /// Image keys this primitive depends on.
    images: SmallVec<[ImageDependency; 8]>,

    /// Opacity bindings this primitive depends on.
    opacity_bindings: SmallVec<[OpacityBinding; 4]>,

    /// Color binding this primitive depends on.
    color_binding: Option<ColorBinding>,

    /// Clips that this primitive depends on.
    clips: SmallVec<[ItemUid; 8]>,

    /// Spatial nodes references by the clip dependencies of this primitive.
    spatial_nodes: SmallVec<[SpatialNodeIndex; 4]>,
}

impl PrimitiveDependencyInfo {
    /// Construct dependency info for a new primitive.
    fn new(
        prim_uid: ItemUid,
        prim_clip_box: PictureBox2D,
    ) -> Self {
        PrimitiveDependencyInfo {
            prim_uid,
            images: SmallVec::new(),
            opacity_bindings: SmallVec::new(),
            color_binding: None,
            prim_clip_box,
            clips: SmallVec::new(),
            spatial_nodes: SmallVec::new(),
        }
    }
}

/// A stable ID for a given tile, to help debugging. These are also used
/// as unique identifiers for tile surfaces when using a native compositor.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Ord, Eq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct TileId(pub usize);

/// Uniquely identifies a tile within a picture cache slice
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Copy, Clone, PartialEq, Hash, Eq)]
pub struct TileKey {
    // Tile index (x,y)
    pub tile_offset: TileOffset,
    // Sub-slice (z)
    pub sub_slice_index: SubSliceIndex,
}

/// A descriptor for the kind of texture that a picture cache tile will
/// be drawn into.
#[derive(Debug)]
pub enum SurfaceTextureDescriptor {
    /// When using the WR compositor, the tile is drawn into an entry
    /// in the WR texture cache.
    TextureCache {
        handle: Option<PictureCacheTextureHandle>,
    },
    /// When using an OS compositor, the tile is drawn into a native
    /// surface identified by arbitrary id.
    Native {
        /// The arbitrary id of this tile.
        id: Option<NativeTileId>,
    },
}

/// This is the same as a `SurfaceTextureDescriptor` but has been resolved
/// into a texture cache handle (if appropriate) that can be used by the
/// batching and compositing code in the renderer.
#[derive(Clone, Debug, Eq, PartialEq, Hash)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum ResolvedSurfaceTexture {
    TextureCache {
        /// The texture ID to draw to.
        texture: TextureSource,
    },
    Native {
        /// The arbitrary id of this tile.
        id: NativeTileId,
        /// The size of the tile in device pixels.
        size: DeviceIntSize,
    }
}

impl SurfaceTextureDescriptor {
    /// Create a resolved surface texture for this descriptor
    pub fn resolve(
        &self,
        resource_cache: &ResourceCache,
        size: DeviceIntSize,
    ) -> ResolvedSurfaceTexture {
        match self {
            SurfaceTextureDescriptor::TextureCache { handle } => {
                let texture = resource_cache
                    .picture_textures
                    .get_texture_source(handle.as_ref().unwrap());

                ResolvedSurfaceTexture::TextureCache { texture }
            }
            SurfaceTextureDescriptor::Native { id } => {
                ResolvedSurfaceTexture::Native {
                    id: id.expect("bug: native surface not allocated"),
                    size,
                }
            }
        }
    }
}

/// The backing surface for this tile.
#[derive(Debug)]
pub enum TileSurface {
    Texture {
        /// Descriptor for the surface that this tile draws into.
        descriptor: SurfaceTextureDescriptor,
    },
    Color {
        color: ColorF,
    },
    Clear,
}

impl TileSurface {
    fn kind(&self) -> &'static str {
        match *self {
            TileSurface::Color { .. } => "Color",
            TileSurface::Texture { .. } => "Texture",
            TileSurface::Clear => "Clear",
        }
    }
}

/// Optional extra information returned by is_same when
/// logging is enabled.
#[derive(Debug, Copy, Clone, PartialEq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum CompareHelperResult<T> {
    /// Primitives match
    Equal,
    /// Counts differ
    Count {
        prev_count: u8,
        curr_count: u8,
    },
    /// Sentinel
    Sentinel,
    /// Two items are not equal
    NotEqual {
        prev: T,
        curr: T,
    },
    /// User callback returned true on item
    PredicateTrue {
        curr: T
    },
}

/// The result of a primitive dependency comparison. Size is a u8
/// since this is a hot path in the code, and keeping the data small
/// is a performance win.
#[derive(Debug, Copy, Clone, PartialEq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[repr(u8)]
pub enum PrimitiveCompareResult {
    /// Primitives match
    Equal,
    /// Something in the PrimitiveDescriptor was different
    Descriptor,
    /// The clip node content or spatial node changed
    Clip,
    /// The value of the transform changed
    Transform,
    /// An image dependency was dirty
    Image,
    /// The value of an opacity binding changed
    OpacityBinding,
    /// The value of a color binding changed
    ColorBinding,
}

/// Debugging information about why a tile was invalidated
#[derive(Debug,Clone)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum InvalidationReason {
    /// The background color changed
    BackgroundColor,
    /// The opaque state of the backing native surface changed
    SurfaceOpacityChanged,
    /// There was no backing texture (evicted or never rendered)
    NoTexture,
    /// There was no backing native surface (never rendered, or recreated)
    NoSurface,
    /// The primitive count in the dependency list was different
    PrimCount,
    /// The content of one of the primitives was different
    Content,
    // The compositor type changed
    CompositorKindChanged,
    // The valid region of the tile changed
    ValidRectChanged,
    // The overall scale of the picture cache changed
    ScaleChanged,
    // The content of the sampling surface changed
    SurfaceContentChanged,
}

/// Information about a cached tile.
pub struct Tile {
    /// The grid position of this tile within the picture cache
    pub tile_offset: TileOffset,
    /// The current world rect of this tile.
    pub world_tile_rect: WorldRect,
    /// The current local rect of this tile.
    pub local_tile_rect: PictureRect,
    /// The picture space dirty rect for this tile.
    pub local_dirty_rect: PictureRect,
    /// The device space dirty rect for this tile.
    /// TODO(gw): We have multiple dirty rects available due to the quadtree above. In future,
    ///           expose these as multiple dirty rects, which will help in some cases.
    pub device_dirty_rect: DeviceRect,
    /// World space rect that contains valid pixels region of this tile.
    pub world_valid_rect: WorldRect,
    /// Device space rect that contains valid pixels region of this tile.
    pub device_valid_rect: DeviceRect,
    /// Uniquely describes the content of this tile, in a way that can be
    /// (reasonably) efficiently hashed and compared.
    pub current_descriptor: TileDescriptor,
    /// The content descriptor for this tile from the previous frame.
    pub prev_descriptor: TileDescriptor,
    /// Handle to the backing surface for this tile.
    pub surface: Option<TileSurface>,
    /// If true, this tile is marked valid, and the existing texture
    /// cache handle can be used. Tiles are invalidated during the
    /// build_dirty_regions method.
    pub is_valid: bool,
    /// If true, this tile intersects with the currently visible screen
    /// rect, and will be drawn.
    pub is_visible: bool,
    /// The tile id is stable between display lists and / or frames,
    /// if the tile is retained. Useful for debugging tile evictions.
    pub id: TileId,
    /// If true, the tile was determined to be opaque, which means blending
    /// can be disabled when drawing it.
    pub is_opaque: bool,
    /// Root node of the quadtree dirty rect tracker.
    root: TileNode,
    /// The last rendered background color on this tile.
    background_color: Option<ColorF>,
    /// The first reason the tile was invalidated this frame.
    invalidation_reason: Option<InvalidationReason>,
    /// The local space valid rect for all primitives that affect this tile.
    pub local_valid_rect: PictureBox2D,
    /// z-buffer id for this tile
    pub z_id: ZBufferId,
    pub sub_graphs: Vec<(PictureRect, Vec<(PictureCompositeMode, SurfaceIndex)>)>,
}

impl Tile {
    /// Construct a new, invalid tile.
    fn new(tile_offset: TileOffset) -> Self {
        let id = TileId(NEXT_TILE_ID.fetch_add(1, Ordering::Relaxed));

        Tile {
            tile_offset,
            local_tile_rect: PictureRect::zero(),
            world_tile_rect: WorldRect::zero(),
            world_valid_rect: WorldRect::zero(),
            device_valid_rect: DeviceRect::zero(),
            local_dirty_rect: PictureRect::zero(),
            device_dirty_rect: DeviceRect::zero(),
            surface: None,
            current_descriptor: TileDescriptor::new(),
            prev_descriptor: TileDescriptor::new(),
            is_valid: false,
            is_visible: false,
            id,
            is_opaque: false,
            root: TileNode::new_leaf(Vec::new()),
            background_color: None,
            invalidation_reason: None,
            local_valid_rect: PictureBox2D::zero(),
            z_id: ZBufferId::invalid(),
            sub_graphs: Vec::new(),
        }
    }

    /// Print debug information about this tile to a tree printer.
    fn print(&self, pt: &mut dyn PrintTreePrinter) {
        pt.new_level(format!("Tile {:?}", self.id));
        pt.add_item(format!("local_tile_rect: {:?}", self.local_tile_rect));
        pt.add_item(format!("background_color: {:?}", self.background_color));
        pt.add_item(format!("invalidation_reason: {:?}", self.invalidation_reason));
        self.current_descriptor.print(pt);
        pt.end_level();
    }

    /// Check if the content of the previous and current tile descriptors match
    fn update_dirty_rects(
        &mut self,
        ctx: &TileUpdateDirtyContext,
        state: &mut TileUpdateDirtyState,
        invalidation_reason: &mut Option<InvalidationReason>,
        frame_context: &FrameVisibilityContext,
    ) -> PictureRect {
        let mut prim_comparer = PrimitiveComparer::new(
            &self.prev_descriptor,
            &self.current_descriptor,
            state.resource_cache,
            state.spatial_node_comparer,
            ctx.opacity_bindings,
            ctx.color_bindings,
        );

        let mut dirty_rect = PictureBox2D::zero();
        self.root.update_dirty_rects(
            &self.prev_descriptor.prims,
            &self.current_descriptor.prims,
            &mut prim_comparer,
            &mut dirty_rect,
            state.compare_cache,
            invalidation_reason,
            frame_context,
        );

        dirty_rect
    }

    /// Invalidate a tile based on change in content. This
    /// must be called even if the tile is not currently
    /// visible on screen. We might be able to improve this
    /// later by changing how ComparableVec is used.
    fn update_content_validity(
        &mut self,
        ctx: &TileUpdateDirtyContext,
        state: &mut TileUpdateDirtyState,
        frame_context: &FrameVisibilityContext,
    ) {
        // Check if the contents of the primitives, clips, and
        // other dependencies are the same.
        state.compare_cache.clear();
        let mut invalidation_reason = None;
        let dirty_rect = self.update_dirty_rects(
            ctx,
            state,
            &mut invalidation_reason,
            frame_context,
        );
        if !dirty_rect.is_empty() {
            self.invalidate(
                Some(dirty_rect),
                invalidation_reason.expect("bug: no invalidation_reason"),
            );
        }
        if ctx.invalidate_all {
            self.invalidate(None, InvalidationReason::ScaleChanged);
        }
        // TODO(gw): We can avoid invalidating the whole tile in some cases here,
        //           but it should be a fairly rare invalidation case.
        if self.current_descriptor.local_valid_rect != self.prev_descriptor.local_valid_rect {
            self.invalidate(None, InvalidationReason::ValidRectChanged);
            state.composite_state.dirty_rects_are_valid = false;
        }
    }

    /// Invalidate this tile. If `invalidation_rect` is None, the entire
    /// tile is invalidated.
    fn invalidate(
        &mut self,
        invalidation_rect: Option<PictureRect>,
        reason: InvalidationReason,
    ) {
        self.is_valid = false;

        match invalidation_rect {
            Some(rect) => {
                self.local_dirty_rect = self.local_dirty_rect.union(&rect);
            }
            None => {
                self.local_dirty_rect = self.local_tile_rect;
            }
        }

        if self.invalidation_reason.is_none() {
            self.invalidation_reason = Some(reason);
        }
    }

    /// Called during pre_update of a tile cache instance. Allows the
    /// tile to setup state before primitive dependency calculations.
    fn pre_update(
        &mut self,
        ctx: &TilePreUpdateContext,
    ) {
        self.local_tile_rect = PictureRect::from_origin_and_size(
            PicturePoint::new(
                self.tile_offset.x as f32 * ctx.tile_size.width,
                self.tile_offset.y as f32 * ctx.tile_size.height,
            ),
            ctx.tile_size,
        );
        // TODO(gw): This is a hack / fix for Box2D::union in euclid not working with
        //           zero sized rect accumulation. Once that lands, we'll revert this
        //           to be zero.
        self.local_valid_rect = PictureBox2D::new(
            PicturePoint::new( 1.0e32,  1.0e32),
            PicturePoint::new(-1.0e32, -1.0e32),
        );
        self.invalidation_reason  = None;
        self.sub_graphs.clear();

        self.world_tile_rect = ctx.pic_to_world_mapper
            .map(&self.local_tile_rect)
            .expect("bug: map local tile rect");

        // Check if this tile is currently on screen.
        self.is_visible = self.world_tile_rect.intersects(&ctx.global_screen_world_rect);

        // If the tile isn't visible, early exit, skipping the normal set up to
        // validate dependencies. Instead, we will only compare the current tile
        // dependencies the next time it comes into view.
        if !self.is_visible {
            return;
        }

        if ctx.background_color != self.background_color {
            self.invalidate(None, InvalidationReason::BackgroundColor);
            self.background_color = ctx.background_color;
        }

        // Clear any dependencies so that when we rebuild them we
        // can compare if the tile has the same content.
        mem::swap(
            &mut self.current_descriptor,
            &mut self.prev_descriptor,
        );
        self.current_descriptor.clear();
        self.root.clear(self.local_tile_rect);

        // Since this tile is determined to be visible, it will get updated
        // dependencies, so update the frame id we are storing dependencies for.
        self.current_descriptor.last_updated_frame_id = ctx.frame_id;
    }

    /// Add dependencies for a given primitive to this tile.
    fn add_prim_dependency(
        &mut self,
        info: &PrimitiveDependencyInfo,
    ) {
        // If this tile isn't currently visible, we don't want to update the dependencies
        // for this tile, as an optimization, since it won't be drawn anyway.
        if !self.is_visible {
            return;
        }

        // Incorporate the bounding rect of the primitive in the local valid rect
        // for this tile. This is used to minimize the size of the scissor rect
        // during rasterization and the draw rect during composition of partial tiles.
        self.local_valid_rect = self.local_valid_rect.union(&info.prim_clip_box);

        // TODO(gw): The prim_clip_rect can be impacted by the clip rect of the display port,
        //           which can cause invalidations when a new display list with changed
        //           display port is received. To work around this, clamp the prim clip rect
        //           to the tile boundaries - if the clip hasn't affected the tile, then the
        //           changed clip can't affect the content of the primitive on this tile.
        //           In future, we could consider supplying the display port clip from Gecko
        //           in a different way (e.g. as a scroll frame clip) which still provides
        //           the desired clip for checkerboarding, but doesn't require this extra
        //           work below.

        // TODO(gw): This is a hot part of the code - we could probably optimize further by:
        //           - Using min/max instead of clamps below (if we guarantee the rects are well formed)

        let tile_p0 = self.local_tile_rect.min;
        let tile_p1 = self.local_tile_rect.max;

        let prim_clip_box = PictureBox2D::new(
            PicturePoint::new(
                clampf(info.prim_clip_box.min.x, tile_p0.x, tile_p1.x),
                clampf(info.prim_clip_box.min.y, tile_p0.y, tile_p1.y),
            ),
            PicturePoint::new(
                clampf(info.prim_clip_box.max.x, tile_p0.x, tile_p1.x),
                clampf(info.prim_clip_box.max.y, tile_p0.y, tile_p1.y),
            ),
        );

        // Update the tile descriptor, used for tile comparison during scene swaps.
        let prim_index = PrimitiveDependencyIndex(self.current_descriptor.prims.len() as u32);

        // Encode the deps for this primitive in the `dep_data` byte buffer
        let dep_offset = self.current_descriptor.dep_data.len() as u32;
        let mut dep_count = 0;

        for clip in &info.clips {
            dep_count += 1;
            poke_into_vec(
                &PrimitiveDependency::Clip {
                    clip: *clip,
                },
                &mut self.current_descriptor.dep_data,
            );
        }

        for spatial_node_index in &info.spatial_nodes {
            dep_count += 1;
            poke_into_vec(
                &PrimitiveDependency::SpatialNode {
                    index: *spatial_node_index,
                },
                &mut self.current_descriptor.dep_data,
            );
        }

        for image in &info.images {
            dep_count += 1;
            poke_into_vec(
                &PrimitiveDependency::Image {
                    image: *image,
                },
                &mut self.current_descriptor.dep_data,
            );
        }

        for binding in &info.opacity_bindings {
            dep_count += 1;
            poke_into_vec(
                &PrimitiveDependency::OpacityBinding {
                    binding: *binding,
                },
                &mut self.current_descriptor.dep_data,
            );
        }

        if let Some(ref binding) = info.color_binding {
            dep_count += 1;
            poke_into_vec(
                &PrimitiveDependency::ColorBinding {
                    binding: *binding,
                },
                &mut self.current_descriptor.dep_data,
            );
        }

        self.current_descriptor.prims.push(PrimitiveDescriptor {
            prim_uid: info.prim_uid,
            prim_clip_box,
            dep_offset,
            dep_count,
        });

        // Add this primitive to the dirty rect quadtree.
        self.root.add_prim(prim_index, &info.prim_clip_box);
    }

    /// Called during tile cache instance post_update. Allows invalidation and dirty
    /// rect calculation after primitive dependencies have been updated.
    fn update_dirty_and_valid_rects(
        &mut self,
        ctx: &TileUpdateDirtyContext,
        state: &mut TileUpdateDirtyState,
        frame_context: &FrameVisibilityContext,
    ) {
        // Ensure peek-poke constraint is met, that `dep_data` is large enough
        ensure_red_zone::<PrimitiveDependency>(&mut self.current_descriptor.dep_data);

        // Register the frame id of this tile with the spatial node comparer, to ensure
        // that it doesn't GC any spatial nodes from the comparer that are referenced
        // by this tile. Must be done before we early exit below, so that we retain
        // spatial node info even for tiles that are currently not visible.
        state.spatial_node_comparer.retain_for_frame(self.current_descriptor.last_updated_frame_id);

        // If tile is not visible, just early out from here - we don't update dependencies
        // so don't want to invalidate, merge, split etc. The tile won't need to be drawn
        // (and thus updated / invalidated) until it is on screen again.
        if !self.is_visible {
            return;
        }

        // Calculate the overall valid rect for this tile.
        self.current_descriptor.local_valid_rect = self.local_valid_rect;

        // TODO(gw): In theory, the local tile rect should always have an
        //           intersection with the overall picture rect. In practice,
        //           due to some accuracy issues with how fract_offset (and
        //           fp accuracy) are used in the calling method, this isn't
        //           always true. In this case, it's safe to set the local
        //           valid rect to zero, which means it will be clipped out
        //           and not affect the scene. In future, we should fix the
        //           accuracy issue above, so that this assumption holds, but
        //           it shouldn't have any noticeable effect on performance
        //           or memory usage (textures should never get allocated).
        self.current_descriptor.local_valid_rect = self.local_tile_rect
            .intersection(&ctx.local_rect)
            .and_then(|r| r.intersection(&self.current_descriptor.local_valid_rect))
            .unwrap_or_else(PictureRect::zero);

        // The device_valid_rect is referenced during `update_content_validity` so it
        // must be updated here first.
        self.world_valid_rect = ctx.pic_to_world_mapper
            .map(&self.current_descriptor.local_valid_rect)
            .expect("bug: map local valid rect");

        // The device rect is guaranteed to be aligned on a device pixel - the round
        // is just to deal with float accuracy. However, the valid rect is not
        // always aligned to a device pixel. To handle this, round out to get all
        // required pixels, and intersect with the tile device rect.
        let device_rect = (self.world_tile_rect * ctx.global_device_pixel_scale).round();
        self.device_valid_rect = (self.world_valid_rect * ctx.global_device_pixel_scale)
            .round_out()
            .intersection(&device_rect)
            .unwrap_or_else(DeviceRect::zero);

        // Invalidate the tile based on the content changing.
        self.update_content_validity(ctx, state, frame_context);
    }

    /// Called during tile cache instance post_update. Allows invalidation and dirty
    /// rect calculation after primitive dependencies have been updated.
    fn post_update(
        &mut self,
        ctx: &TilePostUpdateContext,
        state: &mut TilePostUpdateState,
        frame_context: &FrameVisibilityContext,
    ) {
        // If tile is not visible, just early out from here - we don't update dependencies
        // so don't want to invalidate, merge, split etc. The tile won't need to be drawn
        // (and thus updated / invalidated) until it is on screen again.
        if !self.is_visible {
            return;
        }

        // If there are no primitives there is no need to draw or cache it.
        // Bug 1719232 - The final device valid rect does not always describe a non-empty
        // region. Cull the tile as a workaround.
        if self.current_descriptor.prims.is_empty() || self.device_valid_rect.is_empty() {
            // If there is a native compositor surface allocated for this (now empty) tile
            // it must be freed here, otherwise the stale tile with previous contents will
            // be composited. If the tile subsequently gets new primitives added to it, the
            // surface will be re-allocated when it's added to the composite draw list.
            if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { mut id, .. }, .. }) = self.surface.take() {
                if let Some(id) = id.take() {
                    state.resource_cache.destroy_compositor_tile(id);
                }
            }

            self.is_visible = false;
            return;
        }

        // Check if this tile can be considered opaque. Opacity state must be updated only
        // after all early out checks have been performed. Otherwise, we might miss updating
        // the native surface next time this tile becomes visible.
        let clipped_rect = self.current_descriptor.local_valid_rect
            .intersection(&ctx.local_clip_rect)
            .unwrap_or_else(PictureRect::zero);

        let has_opaque_bg_color = self.background_color.map_or(false, |c| c.a >= 1.0);
        let has_opaque_backdrop = ctx.backdrop.map_or(false, |b| b.opaque_rect.contains_box(&clipped_rect));
        let is_opaque = has_opaque_bg_color || has_opaque_backdrop;

        // Set the correct z_id for this tile
        self.z_id = ctx.z_id;

        if is_opaque != self.is_opaque {
            // If opacity changed, the native compositor surface and all tiles get invalidated.
            // (this does nothing if not using native compositor mode).
            // TODO(gw): This property probably changes very rarely, so it is OK to invalidate
            //           everything in this case. If it turns out that this isn't true, we could
            //           consider other options, such as per-tile opacity (natively supported
            //           on CoreAnimation, and supported if backed by non-virtual surfaces in
            //           DirectComposition).
            if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = self.surface {
                if let Some(id) = id.take() {
                    state.resource_cache.destroy_compositor_tile(id);
                }
            }

            // Invalidate the entire tile to force a redraw.
            self.invalidate(None, InvalidationReason::SurfaceOpacityChanged);
            self.is_opaque = is_opaque;
        }

        // Check if the selected composite mode supports dirty rect updates. For Draw composite
        // mode, we can always update the content with smaller dirty rects, unless there is a
        // driver bug to workaround. For native composite mode, we can only use dirty rects if
        // the compositor supports partial surface updates.
        let (supports_dirty_rects, supports_simple_prims) = match state.composite_state.compositor_kind {
            CompositorKind::Draw { .. } => {
                (frame_context.config.gpu_supports_render_target_partial_update, true)
            }
            CompositorKind::Native { capabilities, .. } => {
                (capabilities.max_update_rects > 0, false)
            }
        };

        // TODO(gw): Consider using smaller tiles and/or tile splits for
        //           native compositors that don't support dirty rects.
        if supports_dirty_rects {
            // Only allow splitting for normal content sized tiles
            if ctx.current_tile_size == state.resource_cache.picture_textures.default_tile_size() {
                let max_split_level = 3;

                // Consider splitting / merging dirty regions
                self.root.maybe_merge_or_split(
                    0,
                    &self.current_descriptor.prims,
                    max_split_level,
                );
            }
        }

        // The dirty rect will be set correctly by now. If the underlying platform
        // doesn't support partial updates, and this tile isn't valid, force the dirty
        // rect to be the size of the entire tile.
        if !self.is_valid && !supports_dirty_rects {
            self.local_dirty_rect = self.local_tile_rect;
        }

        // See if this tile is a simple color, in which case we can just draw
        // it as a rect, and avoid allocating a texture surface and drawing it.
        // TODO(gw): Initial native compositor interface doesn't support simple
        //           color tiles. We can definitely support this in DC, so this
        //           should be added as a follow up.
        let is_simple_prim =
            ctx.backdrop.map_or(false, |b| b.kind.is_some()) &&
            self.current_descriptor.prims.len() == 1 &&
            self.is_opaque &&
            supports_simple_prims;

        // Set up the backing surface for this tile.
        let surface = if is_simple_prim {
            // If we determine the tile can be represented by a color, set the
            // surface unconditionally (this will drop any previously used
            // texture cache backing surface).
            match ctx.backdrop.unwrap().kind {
                Some(BackdropKind::Color { color }) => {
                    TileSurface::Color {
                        color,
                    }
                }
                Some(BackdropKind::Clear) => {
                    TileSurface::Clear
                }
                None => {
                    // This should be prevented by the is_simple_prim check above.
                    unreachable!();
                }
            }
        } else {
            // If this tile will be backed by a surface, we want to retain
            // the texture handle from the previous frame, if possible. If
            // the tile was previously a color, or not set, then just set
            // up a new texture cache handle.
            match self.surface.take() {
                Some(TileSurface::Texture { descriptor }) => {
                    // Reuse the existing descriptor and vis mask
                    TileSurface::Texture {
                        descriptor,
                    }
                }
                Some(TileSurface::Color { .. }) | Some(TileSurface::Clear) | None => {
                    // This is the case where we are constructing a tile surface that
                    // involves drawing to a texture. Create the correct surface
                    // descriptor depending on the compositing mode that will read
                    // the output.
                    let descriptor = match state.composite_state.compositor_kind {
                        CompositorKind::Draw { .. } => {
                            // For a texture cache entry, create an invalid handle that
                            // will be allocated when update_picture_cache is called.
                            SurfaceTextureDescriptor::TextureCache {
                                handle: None,
                            }
                        }
                        CompositorKind::Native { .. } => {
                            // Create a native surface surface descriptor, but don't allocate
                            // a surface yet. The surface is allocated *after* occlusion
                            // culling occurs, so that only visible tiles allocate GPU memory.
                            SurfaceTextureDescriptor::Native {
                                id: None,
                            }
                        }
                    };

                    TileSurface::Texture {
                        descriptor,
                    }
                }
            }
        };

        // Store the current surface backing info for use during batching.
        self.surface = Some(surface);
    }
}

/// Defines a key that uniquely identifies a primitive instance.
#[derive(Debug, Clone)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct PrimitiveDescriptor {
    pub prim_uid: ItemUid,
    pub prim_clip_box: PictureBox2D,
    // TODO(gw): These two fields could be packed as a u24/u8
    pub dep_offset: u32,
    pub dep_count: u32,
}

impl PartialEq for PrimitiveDescriptor {
    fn eq(&self, other: &Self) -> bool {
        const EPSILON: f32 = 0.001;

        if self.prim_uid != other.prim_uid {
            return false;
        }

        if !self.prim_clip_box.min.x.approx_eq_eps(&other.prim_clip_box.min.x, &EPSILON) {
            return false;
        }
        if !self.prim_clip_box.min.y.approx_eq_eps(&other.prim_clip_box.min.y, &EPSILON) {
            return false;
        }
        if !self.prim_clip_box.max.x.approx_eq_eps(&other.prim_clip_box.max.x, &EPSILON) {
            return false;
        }
        if !self.prim_clip_box.max.y.approx_eq_eps(&other.prim_clip_box.max.y, &EPSILON) {
            return false;
        }

        if self.dep_count != other.dep_count {
            return false;
        }

        true
    }
}

/// Uniquely describes the content of this tile, in a way that can be
/// (reasonably) efficiently hashed and compared.
#[cfg_attr(any(feature="capture",feature="replay"), derive(Clone))]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct TileDescriptor {
    /// List of primitive instance unique identifiers. The uid is guaranteed
    /// to uniquely describe the content of the primitive template, while
    /// the other parameters describe the clip chain and instance params.
    prims: Vec<PrimitiveDescriptor>,

    /// Picture space rect that contains valid pixels region of this tile.
    pub local_valid_rect: PictureRect,

    /// The last frame this tile had its dependencies updated (dependency updating is
    /// skipped if a tile is off-screen).
    last_updated_frame_id: FrameId,

    /// Packed per-prim dependency information
    dep_data: Vec<u8>,
}

impl TileDescriptor {
    fn new() -> Self {
        TileDescriptor {
            local_valid_rect: PictureRect::zero(),
            dep_data: Vec::new(),
            prims: Vec::new(),
            last_updated_frame_id: FrameId::INVALID,
        }
    }

    /// Print debug information about this tile descriptor to a tree printer.
    fn print(&self, pt: &mut dyn PrintTreePrinter) {
        pt.new_level("current_descriptor".to_string());

        pt.new_level("prims".to_string());
        for prim in &self.prims {
            pt.new_level(format!("prim uid={}", prim.prim_uid.get_uid()));
            pt.add_item(format!("clip: p0={},{} p1={},{}",
                prim.prim_clip_box.min.x,
                prim.prim_clip_box.min.y,
                prim.prim_clip_box.max.x,
                prim.prim_clip_box.max.y,
            ));
            pt.end_level();
        }
        pt.end_level();

        pt.end_level();
    }

    /// Clear the dependency information for a tile, when the dependencies
    /// are being rebuilt.
    fn clear(&mut self) {
        self.local_valid_rect = PictureRect::zero();
        self.prims.clear();
        self.dep_data.clear();
    }
}

/// Represents the dirty region of a tile cache picture.
#[derive(Clone)]
pub struct DirtyRegion {
    /// The overall dirty rect, a combination of dirty_rects
    pub combined: WorldRect,

    /// Spatial node of the picture cache this region represents
    spatial_node_index: SpatialNodeIndex,
}

impl DirtyRegion {
    /// Construct a new dirty region tracker.
    pub fn new(
        spatial_node_index: SpatialNodeIndex,
    ) -> Self {
        DirtyRegion {
            combined: WorldRect::zero(),
            spatial_node_index,
        }
    }

    /// Reset the dirty regions back to empty
    pub fn reset(
        &mut self,
        spatial_node_index: SpatialNodeIndex,
    ) {
        self.combined = WorldRect::zero();
        self.spatial_node_index = spatial_node_index;
    }

    /// Add a dirty region to the tracker. Returns the visibility mask that corresponds to
    /// this region in the tracker.
    pub fn add_dirty_region(
        &mut self,
        rect_in_pic_space: PictureRect,
        spatial_tree: &SpatialTree,
    ) {
        let map_pic_to_world = SpaceMapper::new_with_target(
            spatial_tree.root_reference_frame_index(),
            self.spatial_node_index,
            WorldRect::max_rect(),
            spatial_tree,
        );

        let world_rect = map_pic_to_world
            .map(&rect_in_pic_space)
            .expect("bug");

        // Include this in the overall dirty rect
        self.combined = self.combined.union(&world_rect);
    }
}

// TODO(gw): Tidy this up by:
//      - Rename Clear variant to something more appropriate to what it does
//      - Add an Other variant for things like opaque gradient backdrops
#[derive(Debug, Copy, Clone)]
pub enum BackdropKind {
    Color {
        color: ColorF,
    },
    Clear,
}

/// Stores information about the calculated opaque backdrop of this slice.
#[derive(Debug, Copy, Clone)]
pub struct BackdropInfo {
    /// The picture space rectangle that is known to be opaque. This is used
    /// to determine where subpixel AA can be used, and where alpha blending
    /// can be disabled.
    pub opaque_rect: PictureRect,
    /// If the backdrop covers the entire slice with an opaque color, this
    /// will be set and can be used as a clear color for the slice's tiles.
    pub spanning_opaque_color: Option<ColorF>,
    /// Kind of the backdrop
    pub kind: Option<BackdropKind>,
    /// The picture space rectangle of the backdrop, if kind is set.
    pub backdrop_rect: PictureRect,
}

impl BackdropInfo {
    fn empty() -> Self {
        BackdropInfo {
            opaque_rect: PictureRect::zero(),
            spanning_opaque_color: None,
            kind: None,
            backdrop_rect: PictureRect::zero(),
        }
    }
}

/// Represents the native surfaces created for a picture cache, if using
/// a native compositor. An opaque and alpha surface is always created,
/// but tiles are added to a surface based on current opacity. If the
/// calculated opacity of a tile changes, the tile is invalidated and
/// attached to a different native surface. This means that we don't
/// need to invalidate the entire surface if only some tiles are changing
/// opacity. It also means we can take advantage of opaque tiles on cache
/// slices where only some of the tiles are opaque. There is an assumption
/// that creating a native surface is cheap, and only when a tile is added
/// to a surface is there a significant cost. This assumption holds true
/// for the current native compositor implementations on Windows and Mac.
pub struct NativeSurface {
    /// Native surface for opaque tiles
    pub opaque: NativeSurfaceId,
    /// Native surface for alpha tiles
    pub alpha: NativeSurfaceId,
}

/// Hash key for an external native compositor surface
#[derive(PartialEq, Eq, Hash)]
pub struct ExternalNativeSurfaceKey {
    /// The YUV/RGB image keys that are used to draw this surface.
    pub image_keys: [ImageKey; 3],
    /// If this is not an 'external' compositor surface created via
    /// Compositor::create_external_surface, this is set to the
    /// current device size of the surface.
    pub size: Option<DeviceIntSize>,
}

/// Information about a native compositor surface cached between frames.
pub struct ExternalNativeSurface {
    /// If true, the surface was used this frame. Used for a simple form
    /// of GC to remove old surfaces.
    pub used_this_frame: bool,
    /// The native compositor surface handle
    pub native_surface_id: NativeSurfaceId,
    /// List of image keys, and current image generations, that are drawn in this surface.
    /// The image generations are used to check if the compositor surface is dirty and
    /// needs to be updated.
    pub image_dependencies: [ImageDependency; 3],
}

/// The key that identifies a tile cache instance. For now, it's simple the index of
/// the slice as it was created during scene building.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SliceId(usize);

impl SliceId {
    pub fn new(index: usize) -> Self {
        SliceId(index)
    }
}

/// Information that is required to reuse or create a new tile cache. Created
/// during scene building and passed to the render backend / frame builder.
pub struct TileCacheParams {
    // Index of the slice (also effectively the key of the tile cache, though we use SliceId where that matters)
    pub slice: usize,
    // Flags describing content of this cache (e.g. scrollbars)
    pub slice_flags: SliceFlags,
    // The anchoring spatial node / scroll root
    pub spatial_node_index: SpatialNodeIndex,
    // Optional background color of this tilecache. If present, can be used as an optimization
    // to enable opaque blending and/or subpixel AA in more places.
    pub background_color: Option<ColorF>,
    // Node in the clip-tree that defines where we exclude clips from child prims
    pub shared_clip_node_id: ClipNodeId,
    // Clip leaf that is used to build the clip-chain for this tile cache.
    pub shared_clip_leaf_id: Option<ClipLeafId>,
    // Virtual surface sizes are always square, so this represents both the width and height
    pub virtual_surface_size: i32,
    // The number of compositor surfaces that are being requested for this tile cache.
    // This is only a suggestion - the tile cache will clamp this as a reasonable number
    // and only promote a limited number of surfaces.
    pub compositor_surface_count: usize,
}

/// Defines which sub-slice (effectively a z-index) a primitive exists on within
/// a picture cache instance.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash)]
pub struct SubSliceIndex(u8);

impl SubSliceIndex {
    pub const DEFAULT: SubSliceIndex = SubSliceIndex(0);

    pub fn new(index: usize) -> Self {
        SubSliceIndex(index as u8)
    }

    /// Return true if this sub-slice is the primary sub-slice (for now, we assume
    /// that only the primary sub-slice may be opaque and support subpixel AA, for example).
    pub fn is_primary(&self) -> bool {
        self.0 == 0
    }

    /// Get an array index for this sub-slice
    pub fn as_usize(&self) -> usize {
        self.0 as usize
    }
}

/// Wrapper struct around an external surface descriptor with a little more information
/// that the picture caching code needs.
pub struct CompositorSurface {
    // External surface descriptor used by compositing logic
    pub descriptor: ExternalSurfaceDescriptor,
    // The compositor surface rect + any intersecting prims. Later prims that intersect
    // with this must be added to the next sub-slice.
    prohibited_rect: PictureRect,
    // If the compositor surface content is opaque.
    pub is_opaque: bool,
}

/// A SubSlice represents a potentially overlapping set of tiles within a picture cache. Most
/// picture cache instances will have only a single sub-slice. The exception to this is when
/// a picture cache has compositor surfaces, in which case sub slices are used to interleave
/// content under or order the compositor surface(s).
pub struct SubSlice {
    /// Hash of tiles present in this picture.
    pub tiles: FastHashMap<TileOffset, Box<Tile>>,
    /// The allocated compositor surfaces for this picture cache. May be None if
    /// not using native compositor, or if the surface was destroyed and needs
    /// to be reallocated next time this surface contains valid tiles.
    pub native_surface: Option<NativeSurface>,
    /// List of compositor surfaces that have been promoted from primitives
    /// in this tile cache.
    pub compositor_surfaces: Vec<CompositorSurface>,
    /// List of visible tiles to be composited for this subslice
    pub composite_tiles: Vec<CompositeTile>,
    /// Compositor descriptors of visible, opaque tiles (used by composite_state.push_surface)
    pub opaque_tile_descriptors: Vec<CompositeTileDescriptor>,
    /// Compositor descriptors of visible, alpha tiles (used by composite_state.push_surface)
    pub alpha_tile_descriptors: Vec<CompositeTileDescriptor>,
}

impl SubSlice {
    /// Construct a new sub-slice
    fn new() -> Self {
        SubSlice {
            tiles: FastHashMap::default(),
            native_surface: None,
            compositor_surfaces: Vec::new(),
            composite_tiles: Vec::new(),
            opaque_tile_descriptors: Vec::new(),
            alpha_tile_descriptors: Vec::new(),
        }
    }

    /// Reset the list of compositor surfaces that follow this sub-slice.
    /// Built per-frame, since APZ may change whether an image is suitable to be a compositor surface.
    fn reset(&mut self) {
        self.compositor_surfaces.clear();
        self.composite_tiles.clear();
        self.opaque_tile_descriptors.clear();
        self.alpha_tile_descriptors.clear();
    }

    /// Resize the tile grid to match a new tile bounds
    fn resize(&mut self, new_tile_rect: TileRect) -> FastHashMap<TileOffset, Box<Tile>> {
        let mut old_tiles = mem::replace(&mut self.tiles, FastHashMap::default());
        self.tiles.reserve(new_tile_rect.area() as usize);

        for y in new_tile_rect.min.y .. new_tile_rect.max.y {
            for x in new_tile_rect.min.x .. new_tile_rect.max.x {
                let key = TileOffset::new(x, y);
                let tile = old_tiles
                    .remove(&key)
                    .unwrap_or_else(|| {
                        Box::new(Tile::new(key))
                    });
                self.tiles.insert(key, tile);
            }
        }

        old_tiles
    }
}

pub struct BackdropSurface {
    pub id: NativeSurfaceId,
    color: ColorF,
    pub device_rect: DeviceRect,
}

/// Represents a cache of tiles that make up a picture primitives.
pub struct TileCacheInstance {
    /// Index of the tile cache / slice for this frame builder. It's determined
    /// by the setup_picture_caching method during flattening, which splits the
    /// picture tree into multiple slices. It's used as a simple input to the tile
    /// keys. It does mean we invalidate tiles if a new layer gets inserted / removed
    /// between display lists - this seems very unlikely to occur on most pages, but
    /// can be revisited if we ever notice that.
    pub slice: usize,
    /// Propagated information about the slice
    pub slice_flags: SliceFlags,
    /// The currently selected tile size to use for this cache
    pub current_tile_size: DeviceIntSize,
    /// The list of sub-slices in this tile cache
    pub sub_slices: Vec<SubSlice>,
    /// The positioning node for this tile cache.
    pub spatial_node_index: SpatialNodeIndex,
    /// List of opacity bindings, with some extra information
    /// about whether they changed since last frame.
    opacity_bindings: FastHashMap<PropertyBindingId, OpacityBindingInfo>,
    /// Switch back and forth between old and new bindings hashmaps to avoid re-allocating.
    old_opacity_bindings: FastHashMap<PropertyBindingId, OpacityBindingInfo>,
    /// A helper to compare transforms between previous and current frame.
    spatial_node_comparer: SpatialNodeComparer,
    /// List of color bindings, with some extra information
    /// about whether they changed since last frame.
    color_bindings: FastHashMap<PropertyBindingId, ColorBindingInfo>,
    /// Switch back and forth between old and new bindings hashmaps to avoid re-allocating.
    old_color_bindings: FastHashMap<PropertyBindingId, ColorBindingInfo>,
    /// The current dirty region tracker for this picture.
    pub dirty_region: DirtyRegion,
    /// Current size of tiles in picture units.
    tile_size: PictureSize,
    /// Tile coords of the currently allocated grid.
    tile_rect: TileRect,
    /// Pre-calculated versions of the tile_rect above, used to speed up the
    /// calculations in get_tile_coords_for_rect.
    tile_bounds_p0: TileOffset,
    tile_bounds_p1: TileOffset,
    /// Local rect (unclipped) of the picture this cache covers.
    pub local_rect: PictureRect,
    /// The local clip rect, from the shared clips of this picture.
    pub local_clip_rect: PictureRect,
    /// The screen rect, transformed to local picture space.
    pub screen_rect_in_pic_space: PictureRect,
    /// The surface index that this tile cache will be drawn into.
    surface_index: SurfaceIndex,
    /// The background color from the renderer. If this is set opaque, we know it's
    /// fine to clear the tiles to this and allow subpixel text on the first slice.
    pub background_color: Option<ColorF>,
    /// Information about the calculated backdrop content of this cache.
    pub backdrop: BackdropInfo,
    /// The allowed subpixel mode for this surface, which depends on the detected
    /// opacity of the background.
    pub subpixel_mode: SubpixelMode,
    // Node in the clip-tree that defines where we exclude clips from child prims
    pub shared_clip_node_id: ClipNodeId,
    // Clip leaf that is used to build the clip-chain for this tile cache.
    pub shared_clip_leaf_id: Option<ClipLeafId>,
    /// The number of frames until this cache next evaluates what tile size to use.
    /// If a picture rect size is regularly changing just around a size threshold,
    /// we don't want to constantly invalidate and reallocate different tile size
    /// configuration each frame.
    frames_until_size_eval: usize,
    /// For DirectComposition, virtual surfaces don't support negative coordinates. However,
    /// picture cache tile coordinates can be negative. To handle this, we apply an offset
    /// to each tile in DirectComposition. We want to change this as little as possible,
    /// to avoid invalidating tiles. However, if we have a picture cache tile coordinate
    /// which is outside the virtual surface bounds, we must change this to allow
    /// correct remapping of the coordinates passed to BeginDraw in DC.
    virtual_offset: DeviceIntPoint,
    /// keep around the hash map used as compare_cache to avoid reallocating it each
    /// frame.
    compare_cache: FastHashMap<PrimitiveComparisonKey, PrimitiveCompareResult>,
    /// The currently considered tile size override. Used to check if we should
    /// re-evaluate tile size, even if the frame timer hasn't expired.
    tile_size_override: Option<DeviceIntSize>,
    /// A cache of compositor surfaces that are retained between frames
    pub external_native_surface_cache: FastHashMap<ExternalNativeSurfaceKey, ExternalNativeSurface>,
    /// Current frame ID of this tile cache instance. Used for book-keeping / garbage collecting
    frame_id: FrameId,
    /// Registered transform in CompositeState for this picture cache
    pub transform_index: CompositorTransformIndex,
    /// Current transform mapping local picture space to compositor surface space
    local_to_surface: ScaleOffset,
    /// If true, we need to invalidate all tiles during `post_update`
    invalidate_all_tiles: bool,
    /// Current transform mapping compositor surface space to final device space
    surface_to_device: ScaleOffset,
    /// The current raster scale for tiles in this cache
    current_raster_scale: f32,
    /// Depth of off-screen surfaces that are currently pushed during dependency updates
    current_surface_traversal_depth: usize,
    /// A list of extra dirty invalidation tests that can only be checked once we
    /// know the dirty rect of all tiles
    deferred_dirty_tests: Vec<DeferredDirtyTest>,
    /// Is there a backdrop associated with this cache
    found_prims_after_backdrop: bool,
    pub backdrop_surface: Option<BackdropSurface>,
}

enum SurfacePromotionResult {
    Failed,
    Success,
}

impl TileCacheInstance {
    pub fn new(params: TileCacheParams) -> Self {
        // Determine how many sub-slices we need. Clamp to an arbitrary limit to ensure
        // we don't create a huge number of OS compositor tiles and sub-slices.
        let sub_slice_count = params.compositor_surface_count.min(MAX_COMPOSITOR_SURFACES) + 1;

        let mut sub_slices = Vec::with_capacity(sub_slice_count);
        for _ in 0 .. sub_slice_count {
            sub_slices.push(SubSlice::new());
        }

        TileCacheInstance {
            slice: params.slice,
            slice_flags: params.slice_flags,
            spatial_node_index: params.spatial_node_index,
            sub_slices,
            opacity_bindings: FastHashMap::default(),
            old_opacity_bindings: FastHashMap::default(),
            spatial_node_comparer: SpatialNodeComparer::new(),
            color_bindings: FastHashMap::default(),
            old_color_bindings: FastHashMap::default(),
            dirty_region: DirtyRegion::new(params.spatial_node_index),
            tile_size: PictureSize::zero(),
            tile_rect: TileRect::zero(),
            tile_bounds_p0: TileOffset::zero(),
            tile_bounds_p1: TileOffset::zero(),
            local_rect: PictureRect::zero(),
            local_clip_rect: PictureRect::zero(),
            screen_rect_in_pic_space: PictureRect::zero(),
            surface_index: SurfaceIndex(0),
            background_color: params.background_color,
            backdrop: BackdropInfo::empty(),
            subpixel_mode: SubpixelMode::Allow,
            shared_clip_node_id: params.shared_clip_node_id,
            shared_clip_leaf_id: params.shared_clip_leaf_id,
            current_tile_size: DeviceIntSize::zero(),
            frames_until_size_eval: 0,
            // Default to centering the virtual offset in the middle of the DC virtual surface
            virtual_offset: DeviceIntPoint::new(
                params.virtual_surface_size / 2,
                params.virtual_surface_size / 2,
            ),
            compare_cache: FastHashMap::default(),
            tile_size_override: None,
            external_native_surface_cache: FastHashMap::default(),
            frame_id: FrameId::INVALID,
            transform_index: CompositorTransformIndex::INVALID,
            surface_to_device: ScaleOffset::identity(),
            local_to_surface: ScaleOffset::identity(),
            invalidate_all_tiles: true,
            current_raster_scale: 1.0,
            current_surface_traversal_depth: 0,
            deferred_dirty_tests: Vec::new(),
            found_prims_after_backdrop: false,
            backdrop_surface: None,
        }
    }

    /// Return the total number of tiles allocated by this tile cache
    pub fn tile_count(&self) -> usize {
        self.tile_rect.area() as usize * self.sub_slices.len()
    }

    /// Trims memory held by the tile cache, such as native surfaces.
    pub fn memory_pressure(&mut self, resource_cache: &mut ResourceCache) {
        for sub_slice in &mut self.sub_slices {
            for tile in sub_slice.tiles.values_mut() {
                if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = tile.surface {
                    // Reseting the id to None with take() ensures that a new
                    // tile will be allocated during the next frame build.
                    if let Some(id) = id.take() {
                        resource_cache.destroy_compositor_tile(id);
                    }
                }
            }
            if let Some(native_surface) = sub_slice.native_surface.take() {
                resource_cache.destroy_compositor_surface(native_surface.opaque);
                resource_cache.destroy_compositor_surface(native_surface.alpha);
            }
        }
    }

    /// Reset this tile cache with the updated parameters from a new scene
    /// that has arrived. This allows the tile cache to be retained across
    /// new scenes.
    pub fn prepare_for_new_scene(
        &mut self,
        params: TileCacheParams,
        resource_cache: &mut ResourceCache,
    ) {
        // We should only receive updated state for matching slice key
        assert_eq!(self.slice, params.slice);

        // Determine how many sub-slices we need, based on how many compositor surface prims are
        // in the supplied primitive list.
        let required_sub_slice_count = params.compositor_surface_count.min(MAX_COMPOSITOR_SURFACES) + 1;

        if self.sub_slices.len() != required_sub_slice_count {
            self.tile_rect = TileRect::zero();

            if self.sub_slices.len() > required_sub_slice_count {
                let old_sub_slices = self.sub_slices.split_off(required_sub_slice_count);

                for mut sub_slice in old_sub_slices {
                    for tile in sub_slice.tiles.values_mut() {
                        if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = tile.surface {
                            if let Some(id) = id.take() {
                                resource_cache.destroy_compositor_tile(id);
                            }
                        }
                    }

                    if let Some(native_surface) = sub_slice.native_surface {
                        resource_cache.destroy_compositor_surface(native_surface.opaque);
                        resource_cache.destroy_compositor_surface(native_surface.alpha);
                    }
                }
            } else {
                while self.sub_slices.len() < required_sub_slice_count {
                    self.sub_slices.push(SubSlice::new());
                }
            }
        }

        // Store the parameters from the scene builder for this slice. Other
        // params in the tile cache are retained and reused, or are always
        // updated during pre/post_update.
        self.slice_flags = params.slice_flags;
        self.spatial_node_index = params.spatial_node_index;
        self.background_color = params.background_color;
        self.shared_clip_leaf_id = params.shared_clip_leaf_id;
        self.shared_clip_node_id = params.shared_clip_node_id;

        // Since the slice flags may have changed, ensure we re-evaluate the
        // appropriate tile size for this cache next update.
        self.frames_until_size_eval = 0;
    }

    /// Destroy any manually managed resources before this picture cache is
    /// destroyed, such as native compositor surfaces.
    pub fn destroy(
        self,
        resource_cache: &mut ResourceCache,
    ) {
        for sub_slice in self.sub_slices {
            if let Some(native_surface) = sub_slice.native_surface {
                resource_cache.destroy_compositor_surface(native_surface.opaque);
                resource_cache.destroy_compositor_surface(native_surface.alpha);
            }
        }

        for (_, external_surface) in self.external_native_surface_cache {
            resource_cache.destroy_compositor_surface(external_surface.native_surface_id)
        }

        if let Some(backdrop_surface) = &self.backdrop_surface {
            resource_cache.destroy_compositor_surface(backdrop_surface.id);
        }
    }

    /// Get the tile coordinates for a given rectangle.
    fn get_tile_coords_for_rect(
        &self,
        rect: &PictureRect,
    ) -> (TileOffset, TileOffset) {
        // Get the tile coordinates in the picture space.
        let mut p0 = TileOffset::new(
            (rect.min.x / self.tile_size.width).floor() as i32,
            (rect.min.y / self.tile_size.height).floor() as i32,
        );

        let mut p1 = TileOffset::new(
            (rect.max.x / self.tile_size.width).ceil() as i32,
            (rect.max.y / self.tile_size.height).ceil() as i32,
        );

        // Clamp the tile coordinates here to avoid looping over irrelevant tiles later on.
        p0.x = clamp(p0.x, self.tile_bounds_p0.x, self.tile_bounds_p1.x);
        p0.y = clamp(p0.y, self.tile_bounds_p0.y, self.tile_bounds_p1.y);
        p1.x = clamp(p1.x, self.tile_bounds_p0.x, self.tile_bounds_p1.x);
        p1.y = clamp(p1.y, self.tile_bounds_p0.y, self.tile_bounds_p1.y);

        (p0, p1)
    }

    /// Update transforms, opacity, color bindings and tile rects.
    pub fn pre_update(
        &mut self,
        pic_rect: PictureRect,
        surface_index: SurfaceIndex,
        frame_context: &FrameVisibilityContext,
        frame_state: &mut FrameVisibilityState,
    ) -> WorldRect {
        self.surface_index = surface_index;
        self.local_rect = pic_rect;
        self.local_clip_rect = PictureRect::max_rect();
        self.deferred_dirty_tests.clear();

        for sub_slice in &mut self.sub_slices {
            sub_slice.reset();
        }

        // Reset the opaque rect + subpixel mode, as they are calculated
        // during the prim dependency checks.
        self.backdrop = BackdropInfo::empty();

        // Calculate the screen rect in picture space, for later comparison against
        // backdrops, and prims potentially covering backdrops.
        let pic_to_world_mapper = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            self.spatial_node_index,
            frame_context.global_screen_world_rect,
            frame_context.spatial_tree,
        );
        self.screen_rect_in_pic_space = pic_to_world_mapper
            .unmap(&frame_context.global_screen_world_rect)
            .expect("unable to unmap screen rect");

        // If there is a valid set of shared clips, build a clip chain instance for this,
        // which will provide a local clip rect. This is useful for establishing things
        // like whether the backdrop rect supplied by Gecko can be considered opaque.
        if let Some(shared_clip_leaf_id) = self.shared_clip_leaf_id {
            let map_local_to_surface = SpaceMapper::new(
                self.spatial_node_index,
                pic_rect,
            );

            frame_state.clip_store.set_active_clips(
                self.spatial_node_index,
                map_local_to_surface.ref_spatial_node_index,
                shared_clip_leaf_id,
                frame_context.spatial_tree,
                &mut frame_state.data_stores.clip,
                &frame_state.clip_tree,
            );

            let clip_chain_instance = frame_state.clip_store.build_clip_chain_instance(
                pic_rect.cast_unit(),
                &map_local_to_surface,
                &pic_to_world_mapper,
                frame_context.spatial_tree,
                frame_state.gpu_cache,
                frame_state.resource_cache,
                frame_context.global_device_pixel_scale,
                &frame_context.global_screen_world_rect,
                &mut frame_state.data_stores.clip,
                true,
            );

            // Ensure that if the entire picture cache is clipped out, the local
            // clip rect is zero. This makes sure we don't register any occluders
            // that are actually off-screen.
            self.local_clip_rect = clip_chain_instance.map_or(PictureRect::zero(), |clip_chain_instance| {
                clip_chain_instance.pic_coverage_rect
            });
        }

        // Advance the current frame ID counter for this picture cache (must be done
        // after any retained prev state is taken above).
        self.frame_id.advance();

        // Notify the spatial node comparer that a new frame has started, and the
        // current reference spatial node for this tile cache.
        self.spatial_node_comparer.next_frame(self.spatial_node_index);

        // At the start of the frame, step through each current compositor surface
        // and mark it as unused. Later, this is used to free old compositor surfaces.
        // TODO(gw): In future, we might make this more sophisticated - for example,
        //           retaining them for >1 frame if unused, or retaining them in some
        //           kind of pool to reduce future allocations.
        for external_native_surface in self.external_native_surface_cache.values_mut() {
            external_native_surface.used_this_frame = false;
        }

        // Only evaluate what tile size to use fairly infrequently, so that we don't end
        // up constantly invalidating and reallocating tiles if the picture rect size is
        // changing near a threshold value.
        if self.frames_until_size_eval == 0 ||
           self.tile_size_override != frame_context.config.tile_size_override {

            // Work out what size tile is appropriate for this picture cache.
            let desired_tile_size = match frame_context.config.tile_size_override {
                Some(tile_size_override) => {
                    tile_size_override
                }
                None => {
                    if self.slice_flags.contains(SliceFlags::IS_SCROLLBAR) {
                        if pic_rect.width() <= pic_rect.height() {
                            TILE_SIZE_SCROLLBAR_VERTICAL
                        } else {
                            TILE_SIZE_SCROLLBAR_HORIZONTAL
                        }
                    } else {
                        frame_state.resource_cache.picture_textures.default_tile_size()
                    }
                }
            };

            // If the desired tile size has changed, then invalidate and drop any
            // existing tiles.
            if desired_tile_size != self.current_tile_size {
                for sub_slice in &mut self.sub_slices {
                    // Destroy any native surfaces on the tiles that will be dropped due
                    // to resizing.
                    if let Some(native_surface) = sub_slice.native_surface.take() {
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.opaque);
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.alpha);
                    }
                    sub_slice.tiles.clear();
                }
                self.tile_rect = TileRect::zero();
                self.current_tile_size = desired_tile_size;
            }

            // Reset counter until next evaluating the desired tile size. This is an
            // arbitrary value.
            self.frames_until_size_eval = 120;
            self.tile_size_override = frame_context.config.tile_size_override;
        }

        // Get the complete scale-offset from local space to device space
        let local_to_device = get_relative_scale_offset(
            self.spatial_node_index,
            frame_context.root_spatial_node_index,
            frame_context.spatial_tree,
        );

        // Get the compositor transform, which depends on pinch-zoom mode
        let mut surface_to_device = local_to_device;

        if frame_context.config.low_quality_pinch_zoom {
            surface_to_device.scale.x /= self.current_raster_scale;
            surface_to_device.scale.y /= self.current_raster_scale;
        } else {
            surface_to_device.scale.x = 1.0;
            surface_to_device.scale.y = 1.0;
        }

        // Use that compositor transform to calculate a relative local to surface
        let local_to_surface = local_to_device.accumulate(&surface_to_device.inverse());

        const EPSILON: f32 = 0.001;
        let compositor_translation_changed =
            !surface_to_device.offset.x.approx_eq_eps(&self.surface_to_device.offset.x, &EPSILON) ||
            !surface_to_device.offset.y.approx_eq_eps(&self.surface_to_device.offset.y, &EPSILON);
        let compositor_scale_changed =
            !surface_to_device.scale.x.approx_eq_eps(&self.surface_to_device.scale.x, &EPSILON) ||
            !surface_to_device.scale.y.approx_eq_eps(&self.surface_to_device.scale.y, &EPSILON);
        let surface_scale_changed =
            !local_to_surface.scale.x.approx_eq_eps(&self.local_to_surface.scale.x, &EPSILON) ||
            !local_to_surface.scale.y.approx_eq_eps(&self.local_to_surface.scale.y, &EPSILON);

        if compositor_translation_changed ||
           compositor_scale_changed ||
           surface_scale_changed ||
           frame_context.config.force_invalidation {
            frame_state.composite_state.dirty_rects_are_valid = false;
        }

        self.surface_to_device = surface_to_device;
        self.local_to_surface = local_to_surface;
        self.invalidate_all_tiles = surface_scale_changed || frame_context.config.force_invalidation;

        // Do a hacky diff of opacity binding values from the last frame. This is
        // used later on during tile invalidation tests.
        let current_properties = frame_context.scene_properties.float_properties();
        mem::swap(&mut self.opacity_bindings, &mut self.old_opacity_bindings);

        self.opacity_bindings.clear();
        for (id, value) in current_properties {
            let changed = match self.old_opacity_bindings.get(id) {
                Some(old_property) => !old_property.value.approx_eq(value),
                None => true,
            };
            self.opacity_bindings.insert(*id, OpacityBindingInfo {
                value: *value,
                changed,
            });
        }

        // Do a hacky diff of color binding values from the last frame. This is
        // used later on during tile invalidation tests.
        let current_properties = frame_context.scene_properties.color_properties();
        mem::swap(&mut self.color_bindings, &mut self.old_color_bindings);

        self.color_bindings.clear();
        for (id, value) in current_properties {
            let changed = match self.old_color_bindings.get(id) {
                Some(old_property) => old_property.value != (*value).into(),
                None => true,
            };
            self.color_bindings.insert(*id, ColorBindingInfo {
                value: (*value).into(),
                changed,
            });
        }

        let world_tile_size = WorldSize::new(
            self.current_tile_size.width as f32 / frame_context.global_device_pixel_scale.0,
            self.current_tile_size.height as f32 / frame_context.global_device_pixel_scale.0,
        );

        self.tile_size = PictureSize::new(
            world_tile_size.width / self.local_to_surface.scale.x,
            world_tile_size.height / self.local_to_surface.scale.y,
        );

        // Inflate the needed rect a bit, so that we retain tiles that we have drawn
        // but have just recently gone off-screen. This means that we avoid re-drawing
        // tiles if the user is scrolling up and down small amounts, at the cost of
        // a bit of extra texture memory.
        let desired_rect_in_pic_space = self.screen_rect_in_pic_space
            .inflate(0.0, 1.0 * self.tile_size.height);

        let needed_rect_in_pic_space = desired_rect_in_pic_space
            .intersection(&pic_rect)
            .unwrap_or_else(Box2D::zero);

        let p0 = needed_rect_in_pic_space.min;
        let p1 = needed_rect_in_pic_space.max;

        let x0 = (p0.x / self.tile_size.width).floor() as i32;
        let x1 = (p1.x / self.tile_size.width).ceil() as i32;

        let y0 = (p0.y / self.tile_size.height).floor() as i32;
        let y1 = (p1.y / self.tile_size.height).ceil() as i32;

        let new_tile_rect = TileRect {
            min: TileOffset::new(x0, y0),
            max: TileOffset::new(x1, y1),
        };

        // Determine whether the current bounds of the tile grid will exceed the
        // bounds of the DC virtual surface, taking into account the current
        // virtual offset. If so, we need to invalidate all tiles, and set up
        // a new virtual offset, centered around the current tile grid.

        let virtual_surface_size = frame_context.config.compositor_kind.get_virtual_surface_size();
        // We only need to invalidate in this case if the underlying platform
        // uses virtual surfaces.
        if virtual_surface_size > 0 {
            // Get the extremities of the tile grid after virtual offset is applied
            let tx0 = self.virtual_offset.x + x0 * self.current_tile_size.width;
            let ty0 = self.virtual_offset.y + y0 * self.current_tile_size.height;
            let tx1 = self.virtual_offset.x + (x1+1) * self.current_tile_size.width;
            let ty1 = self.virtual_offset.y + (y1+1) * self.current_tile_size.height;

            let need_new_virtual_offset = tx0 < 0 ||
                                          ty0 < 0 ||
                                          tx1 >= virtual_surface_size ||
                                          ty1 >= virtual_surface_size;

            if need_new_virtual_offset {
                // Calculate a new virtual offset, centered around the middle of the
                // current tile grid. This means we won't need to invalidate and get
                // a new offset for a long time!
                self.virtual_offset = DeviceIntPoint::new(
                    (virtual_surface_size/2) - ((x0 + x1) / 2) * self.current_tile_size.width,
                    (virtual_surface_size/2) - ((y0 + y1) / 2) * self.current_tile_size.height,
                );

                // Invalidate all native tile surfaces. They will be re-allocated next time
                // they are scheduled to be rasterized.
                for sub_slice in &mut self.sub_slices {
                    for tile in sub_slice.tiles.values_mut() {
                        if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = tile.surface {
                            if let Some(id) = id.take() {
                                frame_state.resource_cache.destroy_compositor_tile(id);
                                tile.surface = None;
                                // Invalidate the entire tile to force a redraw.
                                // TODO(gw): Add a new invalidation reason for virtual offset changing
                                tile.invalidate(None, InvalidationReason::CompositorKindChanged);
                            }
                        }
                    }

                    // Destroy the native virtual surfaces. They will be re-allocated next time a tile
                    // that references them is scheduled to draw.
                    if let Some(native_surface) = sub_slice.native_surface.take() {
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.opaque);
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.alpha);
                    }
                }
            }
        }

        // Rebuild the tile grid if the picture cache rect has changed.
        if new_tile_rect != self.tile_rect {
            for sub_slice in &mut self.sub_slices {
                let mut old_tiles = sub_slice.resize(new_tile_rect);

                // When old tiles that remain after the loop, dirty rects are not valid.
                if !old_tiles.is_empty() {
                    frame_state.composite_state.dirty_rects_are_valid = false;
                }

                // Any old tiles that remain after the loop above are going to be dropped. For
                // simple composite mode, the texture cache handle will expire and be collected
                // by the texture cache. For native compositor mode, we need to explicitly
                // invoke a callback to the client to destroy that surface.
                frame_state.composite_state.destroy_native_tiles(
                    old_tiles.values_mut(),
                    frame_state.resource_cache,
                );
            }
        }

        // This is duplicated information from tile_rect, but cached here to avoid
        // redundant calculations during get_tile_coords_for_rect
        self.tile_bounds_p0 = TileOffset::new(x0, y0);
        self.tile_bounds_p1 = TileOffset::new(x1, y1);
        self.tile_rect = new_tile_rect;

        let mut world_culling_rect = WorldRect::zero();

        let mut ctx = TilePreUpdateContext {
            pic_to_world_mapper,
            background_color: self.background_color,
            global_screen_world_rect: frame_context.global_screen_world_rect,
            tile_size: self.tile_size,
            frame_id: self.frame_id,
        };

        // Pre-update each tile
        for sub_slice in &mut self.sub_slices {
            for tile in sub_slice.tiles.values_mut() {
                tile.pre_update(&ctx);

                // Only include the tiles that are currently in view into the world culling
                // rect. This is a very important optimization for a couple of reasons:
                // (1) Primitives that intersect with tiles in the grid that are not currently
                //     visible can be skipped from primitive preparation, clip chain building
                //     and tile dependency updates.
                // (2) When we need to allocate an off-screen surface for a child picture (for
                //     example a CSS filter) we clip the size of the GPU surface to the world
                //     culling rect below (to ensure we draw enough of it to be sampled by any
                //     tiles that reference it). Making the world culling rect only affected
                //     by visible tiles (rather than the entire virtual tile display port) can
                //     result in allocating _much_ smaller GPU surfaces for cases where the
                //     true off-screen surface size is very large.
                if tile.is_visible {
                    world_culling_rect = world_culling_rect.union(&tile.world_tile_rect);
                }
            }

            // The background color can only be applied to the first sub-slice.
            ctx.background_color = None;
        }

        // If compositor mode is changed, need to drop all incompatible tiles.
        match frame_context.config.compositor_kind {
            CompositorKind::Draw { .. } => {
                for sub_slice in &mut self.sub_slices {
                    for tile in sub_slice.tiles.values_mut() {
                        if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = tile.surface {
                            if let Some(id) = id.take() {
                                frame_state.resource_cache.destroy_compositor_tile(id);
                            }
                            tile.surface = None;
                            // Invalidate the entire tile to force a redraw.
                            tile.invalidate(None, InvalidationReason::CompositorKindChanged);
                        }
                    }

                    if let Some(native_surface) = sub_slice.native_surface.take() {
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.opaque);
                        frame_state.resource_cache.destroy_compositor_surface(native_surface.alpha);
                    }
                }

                for (_, external_surface) in self.external_native_surface_cache.drain() {
                    frame_state.resource_cache.destroy_compositor_surface(external_surface.native_surface_id)
                }
            }
            CompositorKind::Native { .. } => {
                // This could hit even when compositor mode is not changed,
                // then we need to check if there are incompatible tiles.
                for sub_slice in &mut self.sub_slices {
                    for tile in sub_slice.tiles.values_mut() {
                        if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::TextureCache { .. }, .. }) = tile.surface {
                            tile.surface = None;
                            // Invalidate the entire tile to force a redraw.
                            tile.invalidate(None, InvalidationReason::CompositorKindChanged);
                        }
                    }
                }
            }
        }

        world_culling_rect
    }

    fn can_promote_to_surface(
        &mut self,
        flags: PrimitiveFlags,
        prim_clip_chain: &ClipChainInstance,
        prim_spatial_node_index: SpatialNodeIndex,
        is_root_tile_cache: bool,
        sub_slice_index: usize,
        frame_context: &FrameVisibilityContext,
    ) -> SurfacePromotionResult {
        // Check if this primitive _wants_ to be promoted to a compositor surface.
        if !flags.contains(PrimitiveFlags::PREFER_COMPOSITOR_SURFACE) {
            return SurfacePromotionResult::Failed;
        }

        // For now, only support a small (arbitrary) number of compositor surfaces.
        if sub_slice_index == MAX_COMPOSITOR_SURFACES {
            return SurfacePromotionResult::Failed;
        }

        // If a complex clip is being applied to this primitive, it can't be
        // promoted directly to a compositor surface (we might be able to
        // do this in limited cases in future, some native compositors do
        // support rounded rect clips, for example)
        if prim_clip_chain.needs_mask {
            return SurfacePromotionResult::Failed;
        }

        // If not on the root picture cache, it has some kind of
        // complex effect (such as a filter, mix-blend-mode or 3d transform).
        if !is_root_tile_cache {
            return SurfacePromotionResult::Failed;
        }

        let mapper : SpaceMapper<PicturePixel, WorldPixel> = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            prim_spatial_node_index,
            frame_context.global_screen_world_rect,
            &frame_context.spatial_tree);
        let transform = mapper.get_transform();
        if !transform.is_2d_scale_translation() {
            return SurfacePromotionResult::Failed;
        }

        if self.slice_flags.contains(SliceFlags::IS_ATOMIC) {
            return SurfacePromotionResult::Failed;
        }

        SurfacePromotionResult::Success
    }

    fn setup_compositor_surfaces_yuv(
        &mut self,
        sub_slice_index: usize,
        prim_info: &mut PrimitiveDependencyInfo,
        flags: PrimitiveFlags,
        local_prim_rect: LayoutRect,
        prim_spatial_node_index: SpatialNodeIndex,
        pic_coverage_rect: PictureRect,
        frame_context: &FrameVisibilityContext,
        image_dependencies: &[ImageDependency;3],
        api_keys: &[ImageKey; 3],
        resource_cache: &mut ResourceCache,
        composite_state: &mut CompositeState,
        gpu_cache: &mut GpuCache,
        image_rendering: ImageRendering,
        color_depth: ColorDepth,
        color_space: YuvRangedColorSpace,
        format: YuvFormat,
    ) -> bool {
        for &key in api_keys {
            if key != ImageKey::DUMMY {
                // TODO: See comment in setup_compositor_surfaces_rgb.
                resource_cache.request_image(ImageRequest {
                        key,
                        rendering: image_rendering,
                        tile: None,
                    },
                    gpu_cache,
                );
            }
        }

        self.setup_compositor_surfaces_impl(
            sub_slice_index,
            prim_info,
            flags,
            local_prim_rect,
            prim_spatial_node_index,
            pic_coverage_rect,
            frame_context,
            ExternalSurfaceDependency::Yuv {
                image_dependencies: *image_dependencies,
                color_space,
                format,
                channel_bit_depth: color_depth.bit_depth(),
            },
            api_keys,
            resource_cache,
            composite_state,
            image_rendering,
            true,
        )
    }

    fn setup_compositor_surfaces_rgb(
        &mut self,
        sub_slice_index: usize,
        prim_info: &mut PrimitiveDependencyInfo,
        flags: PrimitiveFlags,
        local_prim_rect: LayoutRect,
        prim_spatial_node_index: SpatialNodeIndex,
        pic_coverage_rect: PictureRect,
        frame_context: &FrameVisibilityContext,
        image_dependency: ImageDependency,
        api_key: ImageKey,
        resource_cache: &mut ResourceCache,
        composite_state: &mut CompositeState,
        gpu_cache: &mut GpuCache,
        image_rendering: ImageRendering,
    ) -> bool {
        let mut api_keys = [ImageKey::DUMMY; 3];
        api_keys[0] = api_key;

        // TODO: The picture compositing code requires images promoted
        // into their own picture cache slices to be requested every
        // frame even if they are not visible. However the image updates
        // are only reached on the prepare pass for visible primitives.
        // So we make sure to trigger an image request when promoting
        // the image here.
        resource_cache.request_image(ImageRequest {
                key: api_key,
                rendering: image_rendering,
                tile: None,
            },
            gpu_cache,
        );

        let is_opaque = resource_cache.get_image_properties(api_key)
            .map_or(false, |properties| properties.descriptor.is_opaque());

        self.setup_compositor_surfaces_impl(
            sub_slice_index,
            prim_info,
            flags,
            local_prim_rect,
            prim_spatial_node_index,
            pic_coverage_rect,
            frame_context,
            ExternalSurfaceDependency::Rgb {
                image_dependency,
            },
            &api_keys,
            resource_cache,
            composite_state,
            image_rendering,
            is_opaque,
        )
    }

    // returns false if composition is not available for this surface,
    // and the non-compositor path should be used to draw it instead.
    fn setup_compositor_surfaces_impl(
        &mut self,
        sub_slice_index: usize,
        prim_info: &mut PrimitiveDependencyInfo,
        flags: PrimitiveFlags,
        local_prim_rect: LayoutRect,
        prim_spatial_node_index: SpatialNodeIndex,
        pic_coverage_rect: PictureRect,
        frame_context: &FrameVisibilityContext,
        dependency: ExternalSurfaceDependency,
        api_keys: &[ImageKey; 3],
        resource_cache: &mut ResourceCache,
        composite_state: &mut CompositeState,
        image_rendering: ImageRendering,
        is_opaque: bool,
    ) -> bool {
        let map_local_to_surface = SpaceMapper::new_with_target(
            self.spatial_node_index,
            prim_spatial_node_index,
            self.local_rect,
            frame_context.spatial_tree,
        );

        // Map the primitive local rect into picture space.
        let prim_rect = match map_local_to_surface.map(&local_prim_rect) {
            Some(rect) => rect,
            None => return true,
        };

        // If the rect is invalid, no need to create dependencies.
        if prim_rect.is_empty() {
            return true;
        }

        let pic_to_world_mapper = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            self.spatial_node_index,
            frame_context.global_screen_world_rect,
            frame_context.spatial_tree,
        );

        let world_clip_rect = pic_to_world_mapper
            .map(&prim_info.prim_clip_box)
            .expect("bug: unable to map clip to world space");

        let is_visible = world_clip_rect.intersects(&frame_context.global_screen_world_rect);
        if !is_visible {
            return true;
        }

        let prim_offset = ScaleOffset::from_offset(local_prim_rect.min.to_vector().cast_unit());

        let local_prim_to_device = get_relative_scale_offset(
            prim_spatial_node_index,
            frame_context.root_spatial_node_index,
            frame_context.spatial_tree,
        );

        let normalized_prim_to_device = prim_offset.accumulate(&local_prim_to_device);

        let local_to_surface = ScaleOffset::identity();
        let surface_to_device = normalized_prim_to_device;

        // If this primitive is an external image, and supports being used
        // directly by a native compositor, then lookup the external image id
        // so we can pass that through.
        let mut external_image_id = if flags.contains(PrimitiveFlags::SUPPORTS_EXTERNAL_COMPOSITOR_SURFACE)
            && image_rendering == ImageRendering::Auto {
            resource_cache.get_image_properties(api_keys[0])
                .and_then(|properties| properties.external_image)
                .and_then(|image| Some(image.id))
        } else {
            None
        };


        if let CompositorKind::Native { capabilities, .. } = composite_state.compositor_kind {
            if external_image_id.is_some() &&
               !capabilities.supports_external_compositor_surface_negative_scaling &&
               (surface_to_device.scale.x < 0.0 || surface_to_device.scale.y < 0.0) {
                external_image_id = None;
            }
        }

        let compositor_transform_index = composite_state.register_transform(
            local_to_surface,
            surface_to_device,
        );

        let surface_size = composite_state.get_surface_rect(
            &local_prim_rect,
            &local_prim_rect,
            compositor_transform_index,
        ).size();

        let clip_rect = (world_clip_rect * frame_context.global_device_pixel_scale).round();

        if surface_size.width >= MAX_COMPOSITOR_SURFACES_SIZE ||
           surface_size.height >= MAX_COMPOSITOR_SURFACES_SIZE {
           return false;
        }

        // When using native compositing, we need to find an existing native surface
        // handle to use, or allocate a new one. For existing native surfaces, we can
        // also determine whether this needs to be updated, depending on whether the
        // image generation(s) of the planes have changed since last composite.
        let (native_surface_id, update_params) = match composite_state.compositor_kind {
            CompositorKind::Draw { .. } => {
                (None, None)
            }
            CompositorKind::Native { .. } => {
                let native_surface_size = surface_size.to_i32();

                let key = ExternalNativeSurfaceKey {
                    image_keys: *api_keys,
                    size: if external_image_id.is_some() { None } else { Some(native_surface_size) },
                };

                let native_surface = self.external_native_surface_cache
                    .entry(key)
                    .or_insert_with(|| {
                        // No existing surface, so allocate a new compositor surface.
                        let native_surface_id = match external_image_id {
                            Some(_external_image) => {
                                // If we have a suitable external image, then create an external
                                // surface to attach to.
                                resource_cache.create_compositor_external_surface(is_opaque)
                            }
                            None => {
                                // Otherwise create a normal compositor surface and a single
                                // compositor tile that covers the entire surface.
                                let native_surface_id =
                                resource_cache.create_compositor_surface(
                                    DeviceIntPoint::zero(),
                                    native_surface_size,
                                    is_opaque,
                                );

                                let tile_id = NativeTileId {
                                    surface_id: native_surface_id,
                                    x: 0,
                                    y: 0,
                                };
                                resource_cache.create_compositor_tile(tile_id);

                                native_surface_id
                            }
                        };

                        ExternalNativeSurface {
                            used_this_frame: true,
                            native_surface_id,
                            image_dependencies: [ImageDependency::INVALID; 3],
                        }
                    });

                // Mark that the surface is referenced this frame so that the
                // backing native surface handle isn't freed.
                native_surface.used_this_frame = true;

                let update_params = match external_image_id {
                    Some(external_image) => {
                        // If this is an external image surface, then there's no update
                        // to be done. Just attach the current external image to the surface
                        // and we're done.
                        resource_cache.attach_compositor_external_image(
                            native_surface.native_surface_id,
                            external_image,
                        );
                        None
                    }
                    None => {
                        // If the image dependencies match, there is no need to update
                        // the backing native surface.
                        match dependency {
                            ExternalSurfaceDependency::Yuv{ image_dependencies, .. } => {
                                if image_dependencies == native_surface.image_dependencies {
                                    None
                                } else {
                                    Some(native_surface_size)
                                }
                            },
                            ExternalSurfaceDependency::Rgb{ image_dependency, .. } => {
                                if image_dependency == native_surface.image_dependencies[0] {
                                    None
                                } else {
                                    Some(native_surface_size)
                                }
                            },
                        }
                    }
                };

                (Some(native_surface.native_surface_id), update_params)
            }
        };

        // For compositor surfaces, if we didn't find an earlier sub-slice to add to,
        // we know we can append to the current slice.
        assert!(sub_slice_index < self.sub_slices.len() - 1);
        let sub_slice = &mut self.sub_slices[sub_slice_index];

        // Each compositor surface allocates a unique z-id
        sub_slice.compositor_surfaces.push(CompositorSurface {
            prohibited_rect: pic_coverage_rect,
            is_opaque,
            descriptor: ExternalSurfaceDescriptor {
                local_surface_size: local_prim_rect.size(),
                local_rect: prim_rect,
                local_clip_rect: prim_info.prim_clip_box,
                dependency,
                image_rendering,
                clip_rect,
                transform_index: compositor_transform_index,
                z_id: ZBufferId::invalid(),
                native_surface_id,
                update_params,
            },
        });

        true
    }

    /// Push an estimated rect for an off-screen surface during dependency updates. This is
    /// a workaround / hack that allows the picture cache code to know when it should be
    /// processing primitive dependencies as a single atomic unit. In future, we aim to remove
    /// this hack by having the primitive dependencies stored _within_ each owning picture.
    /// This is part of the work required to support child picture caching anyway!
    pub fn push_surface(
        &mut self,
        estimated_local_rect: LayoutRect,
        surface_spatial_node_index: SpatialNodeIndex,
        spatial_tree: &SpatialTree,
    ) {
        // Only need to evaluate sub-slice regions if we have compositor surfaces present
        if self.current_surface_traversal_depth == 0 && self.sub_slices.len() > 1 {
            let map_local_to_surface = SpaceMapper::new_with_target(
                self.spatial_node_index,
                surface_spatial_node_index,
                self.local_rect,
                spatial_tree,
            );

            if let Some(pic_rect) = map_local_to_surface.map(&estimated_local_rect) {
                // Find the first sub-slice we can add this primitive to (we want to add
                // prims to the primary surface if possible, so they get subpixel AA).
                for sub_slice in &mut self.sub_slices {
                    let mut intersects_prohibited_region = false;

                    for surface in &mut sub_slice.compositor_surfaces {
                        if pic_rect.intersects(&surface.prohibited_rect) {
                            surface.prohibited_rect = surface.prohibited_rect.union(&pic_rect);

                            intersects_prohibited_region = true;
                        }
                    }

                    if !intersects_prohibited_region {
                        break;
                    }
                }
            }
        }

        self.current_surface_traversal_depth += 1;
    }

    /// Pop an off-screen surface off the stack during dependency updates
    pub fn pop_surface(&mut self) {
        self.current_surface_traversal_depth -= 1;
    }

    /// Update the dependencies for each tile for a given primitive instance.
    pub fn update_prim_dependencies(
        &mut self,
        prim_instance: &mut PrimitiveInstance,
        prim_spatial_node_index: SpatialNodeIndex,
        local_prim_rect: LayoutRect,
        frame_context: &FrameVisibilityContext,
        data_stores: &DataStores,
        clip_store: &ClipStore,
        pictures: &[PicturePrimitive],
        resource_cache: &mut ResourceCache,
        color_bindings: &ColorBindingStorage,
        surface_stack: &[(PictureIndex, SurfaceIndex)],
        composite_state: &mut CompositeState,
        gpu_cache: &mut GpuCache,
        scratch: &mut PrimitiveScratchBuffer,
        is_root_tile_cache: bool,
        surfaces: &mut [SurfaceInfo],
    ) {
        // This primitive exists on the last element on the current surface stack.
        profile_scope!("update_prim_dependencies");
        let prim_surface_index = surface_stack.last().unwrap().1;
        let prim_clip_chain = &prim_instance.vis.clip_chain;

        // Accumulate the exact (clipped) local rect in to the parent surface
        let mut surface = &mut surfaces[prim_surface_index.0];
        surface.clipped_local_rect = surface.clipped_local_rect.union(&prim_clip_chain.pic_coverage_rect);

        // If the primitive is directly drawn onto this picture cache surface, then
        // the pic_coverage_rect is in the same space. If not, we need to map it from
        // the surface space into the picture cache space.
        let on_picture_surface = prim_surface_index == self.surface_index;
        let pic_coverage_rect = if on_picture_surface {
            prim_clip_chain.pic_coverage_rect
        } else {
            // We want to get the rect in the tile cache surface space that this primitive
            // occupies, in order to enable correct invalidation regions. Each surface
            // that exists in the chain between this primitive and the tile cache surface
            // may have an arbitrary inflation factor (for example, in the case of a series
            // of nested blur elements). To account for this, step through the current
            // surface stack, mapping the primitive rect into each surface space, including
            // the inflation factor from each intermediate surface.
            let mut current_pic_coverage_rect = prim_clip_chain.pic_coverage_rect;
            let mut current_spatial_node_index = surfaces[prim_surface_index.0]
                .surface_spatial_node_index;

            for (pic_index, surface_index) in surface_stack.iter().rev() {
                let surface = &surfaces[surface_index.0];
                let pic = &pictures[pic_index.0];

                let map_local_to_surface = SpaceMapper::new_with_target(
                    surface.surface_spatial_node_index,
                    current_spatial_node_index,
                    surface.unclipped_local_rect,
                    frame_context.spatial_tree,
                );

                // Map the rect into the parent surface, and inflate if this surface requires
                // it. If the rect can't be mapping (e.g. due to an invalid transform) then
                // just bail out from the dependencies and cull this primitive.
                current_pic_coverage_rect = match map_local_to_surface.map(&current_pic_coverage_rect) {
                    Some(rect) => {
                        // TODO(gw): The casts here are a hack. We have some interface inconsistencies
                        //           between layout/picture rects which don't really work with the
                        //           current unit system, since sometimes the local rect of a picture
                        //           is a LayoutRect, and sometimes it's a PictureRect. Consider how
                        //           we can improve this?
                        pic.composite_mode.as_ref().unwrap().get_coverage(
                            surface,
                            Some(rect.cast_unit()),
                        ).cast_unit()
                    }
                    None => {
                        return;
                    }
                };

                current_spatial_node_index = surface.surface_spatial_node_index;
            }

            current_pic_coverage_rect
        };

        // Get the tile coordinates in the picture space.
        let (p0, p1) = self.get_tile_coords_for_rect(&pic_coverage_rect);

        // If the primitive is outside the tiling rects, it's known to not
        // be visible.
        if p0.x == p1.x || p0.y == p1.y {
            return;
        }

        // Build the list of resources that this primitive has dependencies on.
        let mut prim_info = PrimitiveDependencyInfo::new(
            prim_instance.uid(),
            pic_coverage_rect,
        );

        let mut sub_slice_index = self.sub_slices.len() - 1;

        // Only need to evaluate sub-slice regions if we have compositor surfaces present
        if sub_slice_index > 0 {
            // Find the first sub-slice we can add this primitive to (we want to add
            // prims to the primary surface if possible, so they get subpixel AA).
            for (i, sub_slice) in self.sub_slices.iter_mut().enumerate() {
                let mut intersects_prohibited_region = false;

                for surface in &mut sub_slice.compositor_surfaces {
                    if pic_coverage_rect.intersects(&surface.prohibited_rect) {
                        surface.prohibited_rect = surface.prohibited_rect.union(&pic_coverage_rect);

                        intersects_prohibited_region = true;
                    }
                }

                if !intersects_prohibited_region {
                    sub_slice_index = i;
                    break;
                }
            }
        }

        // Include the prim spatial node, if differs relative to cache root.
        if prim_spatial_node_index != self.spatial_node_index {
            prim_info.spatial_nodes.push(prim_spatial_node_index);
        }

        // If there was a clip chain, add any clip dependencies to the list for this tile.
        let clip_instances = &clip_store
            .clip_node_instances[prim_clip_chain.clips_range.to_range()];
        for clip_instance in clip_instances {
            let clip = &data_stores.clip[clip_instance.handle];

            prim_info.clips.push(clip_instance.handle.uid());

            // If the clip has the same spatial node, the relative transform
            // will always be the same, so there's no need to depend on it.
            if clip.item.spatial_node_index != self.spatial_node_index
                && !prim_info.spatial_nodes.contains(&clip.item.spatial_node_index) {
                prim_info.spatial_nodes.push(clip.item.spatial_node_index);
            }
        }

        // Certain primitives may select themselves to be a backdrop candidate, which is
        // then applied below.
        let mut backdrop_candidate = None;

        // For pictures, we don't (yet) know the valid clip rect, so we can't correctly
        // use it to calculate the local bounding rect for the tiles. If we include them
        // then we may calculate a bounding rect that is too large, since it won't include
        // the clip bounds of the picture. Excluding them from the bounding rect here
        // fixes any correctness issues (the clips themselves are considered when we
        // consider the bounds of the primitives that are *children* of the picture),
        // however it does potentially result in some un-necessary invalidations of a
        // tile (in cases where the picture local rect affects the tile, but the clip
        // rect eventually means it doesn't affect that tile).
        // TODO(gw): Get picture clips earlier (during the initial picture traversal
        //           pass) so that we can calculate these correctly.
        match prim_instance.kind {
            PrimitiveInstanceKind::Picture { pic_index,.. } => {
                // Pictures can depend on animated opacity bindings.
                let pic = &pictures[pic_index.0];
                if let Some(PictureCompositeMode::Filter(Filter::Opacity(binding, _))) = pic.composite_mode {
                    prim_info.opacity_bindings.push(binding.into());
                }
            }
            PrimitiveInstanceKind::Rectangle { data_handle, color_binding_index, .. } => {
                // Rectangles can only form a backdrop candidate if they are known opaque.
                // TODO(gw): We could resolve the opacity binding here, but the common
                //           case for background rects is that they don't have animated opacity.
                let color = match data_stores.prim[data_handle].kind {
                    PrimitiveTemplateKind::Rectangle { color, .. } => {
                        frame_context.scene_properties.resolve_color(&color)
                    }
                    _ => unreachable!(),
                };
                if color.a >= 1.0 {
                    backdrop_candidate = Some(BackdropInfo {
                        opaque_rect: pic_coverage_rect,
                        spanning_opaque_color: None,
                        kind: Some(BackdropKind::Color { color }),
                        backdrop_rect: pic_coverage_rect,
                    });
                }

                if color_binding_index != ColorBindingIndex::INVALID {
                    prim_info.color_binding = Some(color_bindings[color_binding_index].into());
                }
            }
            PrimitiveInstanceKind::Image { data_handle, ref mut is_compositor_surface, .. } => {
                let image_key = &data_stores.image[data_handle];
                let image_data = &image_key.kind;

                let mut promote_to_surface = false;
                match self.can_promote_to_surface(image_key.common.flags,
                                                  prim_clip_chain,
                                                  prim_spatial_node_index,
                                                  is_root_tile_cache,
                                                  sub_slice_index,
                                                  frame_context) {
                    SurfacePromotionResult::Failed => {
                    }
                    SurfacePromotionResult::Success => {
                        promote_to_surface = true;
                    }
                }

                // Native OS compositors (DC and CA, at least) support premultiplied alpha
                // only. If we have an image that's not pre-multiplied alpha, we can't promote it.
                if image_data.alpha_type == AlphaType::Alpha {
                    promote_to_surface = false;
                }

                if let Some(image_properties) = resource_cache.get_image_properties(image_data.key) {
                    // For an image to be a possible opaque backdrop, it must:
                    // - Have a valid, opaque image descriptor
                    // - Not use tiling (since they can fail to draw)
                    // - Not having any spacing / padding
                    // - Have opaque alpha in the instance (flattened) color
                    if image_properties.descriptor.is_opaque() &&
                       image_properties.tiling.is_none() &&
                       image_data.tile_spacing == LayoutSize::zero() &&
                       image_data.color.a >= 1.0 {
                        backdrop_candidate = Some(BackdropInfo {
                            opaque_rect: pic_coverage_rect,
                            spanning_opaque_color: None,
                            kind: None,
                            backdrop_rect: PictureRect::zero(),
                        });
                    }
                }

                if promote_to_surface {
                    promote_to_surface = self.setup_compositor_surfaces_rgb(
                        sub_slice_index,
                        &mut prim_info,
                        image_key.common.flags,
                        local_prim_rect,
                        prim_spatial_node_index,
                        pic_coverage_rect,
                        frame_context,
                        ImageDependency {
                            key: image_data.key,
                            generation: resource_cache.get_image_generation(image_data.key),
                        },
                        image_data.key,
                        resource_cache,
                        composite_state,
                        gpu_cache,
                        image_data.image_rendering,
                    );
                }

                *is_compositor_surface = promote_to_surface;

                if promote_to_surface {
                    prim_instance.vis.state = VisibilityState::Culled;
                    return;
                } else {
                    prim_info.images.push(ImageDependency {
                        key: image_data.key,
                        generation: resource_cache.get_image_generation(image_data.key),
                    });
                }
            }
            PrimitiveInstanceKind::YuvImage { data_handle, ref mut is_compositor_surface, .. } => {
                let prim_data = &data_stores.yuv_image[data_handle];
                let mut promote_to_surface = match self.can_promote_to_surface(
                                            prim_data.common.flags,
                                            prim_clip_chain,
                                            prim_spatial_node_index,
                                            is_root_tile_cache,
                                            sub_slice_index,
                                            frame_context) {
                    SurfacePromotionResult::Failed => false,
                    SurfacePromotionResult::Success => true,
                };

                // TODO(gw): When we support RGBA images for external surfaces, we also
                //           need to check if opaque (YUV images are implicitly opaque).

                // If this primitive is being promoted to a surface, construct an external
                // surface descriptor for use later during batching and compositing. We only
                // add the image keys for this primitive as a dependency if this is _not_
                // a promoted surface, since we don't want the tiles to invalidate when the
                // video content changes, if it's a compositor surface!
                if promote_to_surface {
                    // Build dependency for each YUV plane, with current image generation for
                    // later detection of when the composited surface has changed.
                    let mut image_dependencies = [ImageDependency::INVALID; 3];
                    for (key, dep) in prim_data.kind.yuv_key.iter().cloned().zip(image_dependencies.iter_mut()) {
                        *dep = ImageDependency {
                            key,
                            generation: resource_cache.get_image_generation(key),
                        }
                    }

                    promote_to_surface = self.setup_compositor_surfaces_yuv(
                        sub_slice_index,
                        &mut prim_info,
                        prim_data.common.flags,
                        local_prim_rect,
                        prim_spatial_node_index,
                        pic_coverage_rect,
                        frame_context,
                        &image_dependencies,
                        &prim_data.kind.yuv_key,
                        resource_cache,
                        composite_state,
                        gpu_cache,
                        prim_data.kind.image_rendering,
                        prim_data.kind.color_depth,
                        prim_data.kind.color_space.with_range(prim_data.kind.color_range),
                        prim_data.kind.format,
                    );
                }

                // Store on the YUV primitive instance whether this is a promoted surface.
                // This is used by the batching code to determine whether to draw the
                // image to the content tiles, or just a transparent z-write.
                *is_compositor_surface = promote_to_surface;

                if promote_to_surface {
                    prim_instance.vis.state = VisibilityState::Culled;
                    return;
                } else {
                    prim_info.images.extend(
                        prim_data.kind.yuv_key.iter().map(|key| {
                            ImageDependency {
                                key: *key,
                                generation: resource_cache.get_image_generation(*key),
                            }
                        })
                    );
                }
            }
            PrimitiveInstanceKind::ImageBorder { data_handle, .. } => {
                let border_data = &data_stores.image_border[data_handle].kind;
                prim_info.images.push(ImageDependency {
                    key: border_data.request.key,
                    generation: resource_cache.get_image_generation(border_data.request.key),
                });
            }
            PrimitiveInstanceKind::Clear { .. } => {
                backdrop_candidate = Some(BackdropInfo {
                    opaque_rect: pic_coverage_rect,
                    spanning_opaque_color: None,
                    kind: Some(BackdropKind::Clear),
                    backdrop_rect: pic_coverage_rect,        
                });
            }
            PrimitiveInstanceKind::LinearGradient { data_handle, .. }
            | PrimitiveInstanceKind::CachedLinearGradient { data_handle, .. } => {
                let gradient_data = &data_stores.linear_grad[data_handle];
                if gradient_data.stops_opacity.is_opaque
                    && gradient_data.tile_spacing == LayoutSize::zero()
                {
                    backdrop_candidate = Some(BackdropInfo {
                        opaque_rect: pic_coverage_rect,
                        spanning_opaque_color: None,
                        kind: None,
                        backdrop_rect: PictureRect::zero(),
                    });
                }
            }
            PrimitiveInstanceKind::ConicGradient { data_handle, .. } => {
                let gradient_data = &data_stores.conic_grad[data_handle];
                if gradient_data.stops_opacity.is_opaque
                    && gradient_data.tile_spacing == LayoutSize::zero()
                {
                    backdrop_candidate = Some(BackdropInfo {
                        opaque_rect: pic_coverage_rect,
                        spanning_opaque_color: None,
                        kind: None,
                        backdrop_rect: PictureRect::zero(),
                    });
                }
            }
            PrimitiveInstanceKind::RadialGradient { data_handle, .. } => {
                let gradient_data = &data_stores.radial_grad[data_handle];
                if gradient_data.stops_opacity.is_opaque
                    && gradient_data.tile_spacing == LayoutSize::zero()
                {
                    backdrop_candidate = Some(BackdropInfo {
                        opaque_rect: pic_coverage_rect,
                        spanning_opaque_color: None,
                        kind: None,
                        backdrop_rect: PictureRect::zero(),
                    });
                }
            }
            PrimitiveInstanceKind::BackdropCapture { .. } => {}
            PrimitiveInstanceKind::BackdropRender { pic_index, .. } => {
                // If the area that the backdrop covers in the space of the surface it draws on
                // is empty, skip any sub-graph processing. This is not just a performance win,
                // it also ensures that we don't do a deferred dirty test that invalidates a tile
                // even if the tile isn't actually dirty, which can cause panics later in the
                // WR pipeline.
                if !pic_coverage_rect.is_empty() {
                    // Mark that we need the sub-graph this render depends on so that
                    // we don't skip it during the prepare pass
                    scratch.required_sub_graphs.insert(pic_index);

                    // If this is a sub-graph, register the bounds on any affected tiles
                    // so we know how much to expand the content tile by.

                    // Implicitly, we know that any slice with a sub-graph disables compositor
                    // surface promotion, so sub_slice_index will always be 0.
                    debug_assert_eq!(sub_slice_index, 0);
                    let sub_slice = &mut self.sub_slices[sub_slice_index];

                    let mut surface_info = Vec::new();
                    for (pic_index, surface_index) in surface_stack.iter().rev() {
                        let pic = &pictures[pic_index.0];
                        surface_info.push((pic.composite_mode.as_ref().unwrap().clone(), *surface_index));
                    }

                    for y in p0.y .. p1.y {
                        for x in p0.x .. p1.x {
                            let key = TileOffset::new(x, y);
                            let tile = sub_slice.tiles.get_mut(&key).expect("bug: no tile");
                            tile.sub_graphs.push((pic_coverage_rect, surface_info.clone()));
                        }
                    }

                    // For backdrop-filter, we need to check if any of the dirty rects
                    // in tiles that are affected by the filter primitive are dirty.
                    self.deferred_dirty_tests.push(DeferredDirtyTest {
                        tile_rect: TileRect::new(p0, p1),
                        prim_rect: pic_coverage_rect,
                    });
                }
            }
            PrimitiveInstanceKind::LineDecoration { .. } |
            PrimitiveInstanceKind::NormalBorder { .. } |
            PrimitiveInstanceKind::TextRun { .. } => {
                // These don't contribute dependencies
            }
        };

        // Calculate the screen rect in local space. When we calculate backdrops, we
        // care only that they cover the visible rect, and don't have any overlapping
        // prims in the visible rect.
        let visible_local_rect = self.local_rect.intersection(&self.screen_rect_in_pic_space).unwrap_or_default();
        if pic_coverage_rect.intersects(&visible_local_rect) {
            self.found_prims_after_backdrop = true;
        }

        // If this primitive considers itself a backdrop candidate, apply further
        // checks to see if it matches all conditions to be a backdrop.
        let mut vis_flags = PrimitiveVisibilityFlags::empty();
        let sub_slice = &mut self.sub_slices[sub_slice_index];
        if let Some(mut backdrop_candidate) = backdrop_candidate {
            // Update whether the surface that this primitive exists on
            // can be considered opaque. Any backdrop kind other than
            // a clear primitive (e.g. color, gradient, image) can be
            // considered.
            match backdrop_candidate.kind {
                Some(BackdropKind::Color { .. }) | None => {
                    let surface = &mut surfaces[prim_surface_index.0];

                    let is_same_coord_system = frame_context.spatial_tree.is_matching_coord_system(
                        prim_spatial_node_index,
                        surface.surface_spatial_node_index,
                    );

                    // To be an opaque backdrop, it must:
                    // - Be the same coordinate system (axis-aligned)
                    // - Have no clip mask
                    // - Have a rect that covers the surface local rect
                    if is_same_coord_system &&
                       !prim_clip_chain.needs_mask &&
                       prim_clip_chain.pic_coverage_rect.contains_box(&surface.unclipped_local_rect)
                    {
                        // Note that we use `prim_clip_chain.pic_clip_rect` here rather
                        // than `backdrop_candidate.opaque_rect`. The former is in the
                        // local space of the surface, the latter is in the local space
                        // of the top level tile-cache.
                        surface.is_opaque = true;
                    }
                }
                Some(BackdropKind::Clear) => {}
            }

            let is_suitable_backdrop = match backdrop_candidate.kind {
                Some(BackdropKind::Clear) => {
                    // Clear prims are special - they always end up in their own slice,
                    // and always set the backdrop. In future, we hope to completely
                    // remove clear prims, since they don't integrate with the compositing
                    // system cleanly.
                    true
                }
                Some(BackdropKind::Color { .. }) | None => {
                    // Check a number of conditions to see if we can consider this
                    // primitive as an opaque backdrop rect. Several of these are conservative
                    // checks and could be relaxed in future. However, these checks
                    // are quick and capture the common cases of background rects and images.
                    // Specifically, we currently require:
                    //  - The primitive is on the main picture cache surface.
                    //  - Same coord system as picture cache (ensures rects are axis-aligned).
                    //  - No clip masks exist.
                    let same_coord_system = frame_context.spatial_tree.is_matching_coord_system(
                        prim_spatial_node_index,
                        self.spatial_node_index,
                    );

                    same_coord_system && on_picture_surface
                }
            };

            if sub_slice_index == 0 &&
               is_suitable_backdrop &&
               sub_slice.compositor_surfaces.is_empty() {

                // If the backdrop candidate has a clip-mask, try to extract an opaque inner
                // rect that is safe to use for subpixel rendering
                if prim_clip_chain.needs_mask {
                    backdrop_candidate.opaque_rect = clip_store
                        .get_inner_rect_for_clip_chain(
                            prim_clip_chain,
                            &data_stores.clip,
                            frame_context.spatial_tree,
                        )
                        .unwrap_or(PictureRect::zero());
                }

                // We set the backdrop opaque_rect here, indicating the coverage area, which
                // is useful for calculate_subpixel_mode. We will only set the backdrop kind
                // if it covers the visible rect.
                if backdrop_candidate.opaque_rect.contains_box(&self.backdrop.opaque_rect) {
                    self.backdrop.opaque_rect = backdrop_candidate.opaque_rect;
                }

                if let Some(kind) = backdrop_candidate.kind {
                    if backdrop_candidate.opaque_rect.contains_box(&visible_local_rect) {
                        self.found_prims_after_backdrop = false;
                        self.backdrop.kind = Some(kind);
                        self.backdrop.backdrop_rect = backdrop_candidate.opaque_rect;

                        // If we have a color backdrop that spans the entire local rect, mark
                        // the visibility flags of the primitive so it is skipped during batching
                        // (and also clears any previous primitives). Additionally, update our
                        // background color to match the backdrop color, which will ensure that
                        // our tiles are cleared to this color.
                        if let BackdropKind::Color { color } = kind {
                            if backdrop_candidate.opaque_rect.contains_box(&self.local_rect) {
                                vis_flags |= PrimitiveVisibilityFlags::IS_BACKDROP;
                                self.backdrop.spanning_opaque_color = Some(color);
                            }
                        }
                    }
                }
            }
        }

        // Record any new spatial nodes in the used list.
        for spatial_node_index in &prim_info.spatial_nodes {
            self.spatial_node_comparer.register_used_transform(
                *spatial_node_index,
                self.frame_id,
                frame_context.spatial_tree,
            );
        }

        // Normalize the tile coordinates before adding to tile dependencies.
        // For each affected tile, mark any of the primitive dependencies.
        for y in p0.y .. p1.y {
            for x in p0.x .. p1.x {
                // TODO(gw): Convert to 2d array temporarily to avoid hash lookups per-tile?
                let key = TileOffset::new(x, y);
                let tile = sub_slice.tiles.get_mut(&key).expect("bug: no tile");

                tile.add_prim_dependency(&prim_info);
            }
        }

        prim_instance.vis.state = VisibilityState::Visible {
            vis_flags,
            sub_slice_index: SubSliceIndex::new(sub_slice_index),
        };
    }

    /// Print debug information about this picture cache to a tree printer.
    fn print(&self) {
        // TODO(gw): This initial implementation is very basic - just printing
        //           the picture cache state to stdout. In future, we can
        //           make this dump each frame to a file, and produce a report
        //           stating which frames had invalidations. This will allow
        //           diff'ing the invalidation states in a visual tool.
        let mut pt = PrintTree::new("Picture Cache");

        pt.new_level(format!("Slice {:?}", self.slice));

        pt.add_item(format!("background_color: {:?}", self.background_color));

        for (sub_slice_index, sub_slice) in self.sub_slices.iter().enumerate() {
            pt.new_level(format!("SubSlice {:?}", sub_slice_index));

            for y in self.tile_bounds_p0.y .. self.tile_bounds_p1.y {
                for x in self.tile_bounds_p0.x .. self.tile_bounds_p1.x {
                    let key = TileOffset::new(x, y);
                    let tile = &sub_slice.tiles[&key];
                    tile.print(&mut pt);
                }
            }

            pt.end_level();
        }

        pt.end_level();
    }

    fn calculate_subpixel_mode(&self) -> SubpixelMode {
        let has_opaque_bg_color = self.background_color.map_or(false, |c| c.a >= 1.0);

        // If the overall tile cache is known opaque, subpixel AA is allowed everywhere
        if has_opaque_bg_color {
            return SubpixelMode::Allow;
        }

        // If we didn't find any valid opaque backdrop, no subpixel AA allowed
        if self.backdrop.opaque_rect.is_empty() {
            return SubpixelMode::Deny;
        }

        // If the opaque backdrop rect covers the entire tile cache surface,
        // we can allow subpixel AA anywhere, skipping the per-text-run tests
        // later on during primitive preparation.
        if self.backdrop.opaque_rect.contains_box(&self.local_rect) {
            return SubpixelMode::Allow;
        }

        // If none of the simple cases above match, we need test where we can support subpixel AA.
        // TODO(gw): In future, it may make sense to have > 1 inclusion rect,
        //           but this handles the common cases.
        // TODO(gw): If a text run gets animated such that it's moving in a way that is
        //           sometimes intersecting with the video rect, this can result in subpixel
        //           AA flicking on/off for that text run. It's probably very rare, but
        //           something we should handle in future.
        SubpixelMode::Conditional {
            allowed_rect: self.backdrop.opaque_rect,
        }
    }

    /// Apply any updates after prim dependency updates. This applies
    /// any late tile invalidations, and sets up the dirty rect and
    /// set of tile blits.
    pub fn post_update(
        &mut self,
        frame_context: &FrameVisibilityContext,
        frame_state: &mut FrameVisibilityState,
    ) {
        assert!(self.current_surface_traversal_depth == 0);

        self.dirty_region.reset(self.spatial_node_index);
        self.subpixel_mode = self.calculate_subpixel_mode();

        self.transform_index = frame_state.composite_state.register_transform(
            self.local_to_surface,
            // TODO(gw): Once we support scaling of picture cache tiles during compositing,
            //           that transform gets plugged in here!
            self.surface_to_device,
        );

        let map_pic_to_world = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            self.spatial_node_index,
            frame_context.global_screen_world_rect,
            frame_context.spatial_tree,
        );

        // A simple GC of the native external surface cache, to remove and free any
        // surfaces that were not referenced during the update_prim_dependencies pass.
        self.external_native_surface_cache.retain(|_, surface| {
            if !surface.used_this_frame {
                // If we removed an external surface, we need to mark the dirty rects as
                // invalid so a full composite occurs on the next frame.
                frame_state.composite_state.dirty_rects_are_valid = false;

                frame_state.resource_cache.destroy_compositor_surface(surface.native_surface_id);
            }

            surface.used_this_frame
        });

        let pic_to_world_mapper = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            self.spatial_node_index,
            frame_context.global_screen_world_rect,
            frame_context.spatial_tree,
        );

        let ctx = TileUpdateDirtyContext {
            pic_to_world_mapper,
            global_device_pixel_scale: frame_context.global_device_pixel_scale,
            opacity_bindings: &self.opacity_bindings,
            color_bindings: &self.color_bindings,
            local_rect: self.local_rect,
            invalidate_all: self.invalidate_all_tiles,
        };

        let mut state = TileUpdateDirtyState {
            resource_cache: frame_state.resource_cache,
            composite_state: frame_state.composite_state,
            compare_cache: &mut self.compare_cache,
            spatial_node_comparer: &mut self.spatial_node_comparer,
        };

        // Step through each tile and invalidate if the dependencies have changed. Determine
        // the current opacity setting and whether it's changed.
        for sub_slice in &mut self.sub_slices {
            for tile in sub_slice.tiles.values_mut() {
                tile.update_dirty_and_valid_rects(&ctx, &mut state, frame_context);
            }
        }

        // Process any deferred dirty checks
        for sub_slice in &mut self.sub_slices {
            for dirty_test in self.deferred_dirty_tests.drain(..) {
                // Calculate the total dirty rect from all tiles that this primitive affects
                let mut total_dirty_rect = PictureRect::zero();

                for y in dirty_test.tile_rect.min.y .. dirty_test.tile_rect.max.y {
                    for x in dirty_test.tile_rect.min.x .. dirty_test.tile_rect.max.x {
                        let key = TileOffset::new(x, y);
                        let tile = sub_slice.tiles.get_mut(&key).expect("bug: no tile");
                        total_dirty_rect = total_dirty_rect.union(&tile.local_dirty_rect);
                    }
                }

                // If that dirty rect intersects with the local rect of the primitive
                // being checked, invalidate that region in all of the affected tiles.
                // TODO(gw): This is somewhat conservative, we could be more clever
                //           here and avoid invalidating every tile when this changes.
                //           We could also store the dirty rect only when the prim
                //           is encountered, so that we don't invalidate if something
                //           *after* the query in the rendering order affects invalidation.
                if total_dirty_rect.intersects(&dirty_test.prim_rect) {
                    for y in dirty_test.tile_rect.min.y .. dirty_test.tile_rect.max.y {
                        for x in dirty_test.tile_rect.min.x .. dirty_test.tile_rect.max.x {
                            let key = TileOffset::new(x, y);
                            let tile = sub_slice.tiles.get_mut(&key).expect("bug: no tile");
                            tile.invalidate(
                                Some(dirty_test.prim_rect),
                                InvalidationReason::SurfaceContentChanged,
                            );
                        }
                    }
                }
            }
        }

        let mut ctx = TilePostUpdateContext {
            local_clip_rect: self.local_clip_rect,
            backdrop: None,
            current_tile_size: self.current_tile_size,
            z_id: ZBufferId::invalid(),
        };

        let mut state = TilePostUpdateState {
            resource_cache: frame_state.resource_cache,
            composite_state: frame_state.composite_state,
        };

        for (i, sub_slice) in self.sub_slices.iter_mut().enumerate().rev() {
            // The backdrop is only relevant for the first sub-slice
            if i == 0 {
                ctx.backdrop = Some(self.backdrop);
            }

            for compositor_surface in sub_slice.compositor_surfaces.iter_mut().rev() {
                compositor_surface.descriptor.z_id = state.composite_state.z_generator.next();
            }

            ctx.z_id = state.composite_state.z_generator.next();

            for tile in sub_slice.tiles.values_mut() {
                tile.post_update(&ctx, &mut state, frame_context);
            }
        }

        // Register any opaque external compositor surfaces as potential occluders. This
        // is especially useful when viewing video in full-screen mode, as it is
        // able to occlude every background tile (avoiding allocation, rasterizion
        // and compositing).

        for sub_slice in &self.sub_slices {
            for compositor_surface in &sub_slice.compositor_surfaces {
                if compositor_surface.is_opaque {
                    let local_surface_rect = compositor_surface
                        .descriptor
                        .local_rect
                        .intersection(&compositor_surface.descriptor.local_clip_rect)
                        .and_then(|r| {
                            r.intersection(&self.local_clip_rect)
                        });

                    if let Some(local_surface_rect) = local_surface_rect {
                        let world_surface_rect = map_pic_to_world
                            .map(&local_surface_rect)
                            .expect("bug: unable to map external surface to world space");

                        frame_state.composite_state.register_occluder(
                            compositor_surface.descriptor.z_id,
                            world_surface_rect,
                        );
                    }
                }
            }
        }

        // Register the opaque region of this tile cache as an occluder, which
        // is used later in the frame to occlude other tiles.
        if !self.backdrop.opaque_rect.is_empty() {
            let z_id_backdrop = frame_state.composite_state.z_generator.next();

            let backdrop_rect = self.backdrop.opaque_rect
                .intersection(&self.local_rect)
                .and_then(|r| {
                    r.intersection(&self.local_clip_rect)
                });

            if let Some(backdrop_rect) = backdrop_rect {
                let world_backdrop_rect = map_pic_to_world
                    .map(&backdrop_rect)
                    .expect("bug: unable to map backdrop to world space");

                // Since we register the entire backdrop rect, use the opaque z-id for the
                // picture cache slice.
                frame_state.composite_state.register_occluder(
                    z_id_backdrop,
                    world_backdrop_rect,
                );
            }
        }
    }
}

pub struct PictureScratchBuffer {
    surface_stack: Vec<SurfaceIndex>,
}

impl Default for PictureScratchBuffer {
    fn default() -> Self {
        PictureScratchBuffer {
            surface_stack: Vec::new(),
        }
    }
}

impl PictureScratchBuffer {
    pub fn begin_frame(&mut self) {
        self.surface_stack.clear();
    }

    pub fn recycle(&mut self, recycler: &mut Recycler) {
        recycler.recycle_vec(&mut self.surface_stack);
    }
}

#[derive(Debug, Copy, Clone, PartialEq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SurfaceIndex(pub usize);

/// Information about an offscreen surface. For now,
/// it contains information about the size and coordinate
/// system of the surface. In the future, it will contain
/// information about the contents of the surface, which
/// will allow surfaces to be cached / retained between
/// frames and display lists.
pub struct SurfaceInfo {
    /// A local rect defining the size of this surface, in the
    /// coordinate system of the surface itself. This contains
    /// the unclipped bounding rect of child primitives.
    pub unclipped_local_rect: PictureRect,
    /// The local space coverage of child primitives after they are
    /// are clipped to their owning clip-chain.
    pub clipped_local_rect: PictureRect,
    /// If true, we know this surface is completely opaque
    pub is_opaque: bool,
    /// The (conservative) valid part of this surface rect. Used
    /// to reduce the size of render target allocation.
    pub clipping_rect: PictureRect,
    /// Helper structs for mapping local rects in different
    /// coordinate systems into the surface coordinates.
    pub map_local_to_surface: SpaceMapper<LayoutPixel, PicturePixel>,
    /// Defines the positioning node for the surface itself,
    /// and the rasterization root for this surface.
    pub raster_spatial_node_index: SpatialNodeIndex,
    pub surface_spatial_node_index: SpatialNodeIndex,
    /// The device pixel ratio specific to this surface.
    pub device_pixel_scale: DevicePixelScale,
    /// The scale factors of the surface to world transform.
    pub world_scale_factors: (f32, f32),
    /// Local scale factors surface to raster transform
    pub local_scale: (f32, f32),
    /// If true, allow snapping on this and child surfaces
    pub allow_snapping: bool,
}

impl SurfaceInfo {
    pub fn new(
        surface_spatial_node_index: SpatialNodeIndex,
        raster_spatial_node_index: SpatialNodeIndex,
        world_rect: WorldRect,
        spatial_tree: &SpatialTree,
        device_pixel_scale: DevicePixelScale,
        world_scale_factors: (f32, f32),
        local_scale: (f32, f32),
        allow_snapping: bool,
    ) -> Self {
        let map_surface_to_world = SpaceMapper::new_with_target(
            spatial_tree.root_reference_frame_index(),
            surface_spatial_node_index,
            world_rect,
            spatial_tree,
        );

        let pic_bounds = map_surface_to_world
            .unmap(&map_surface_to_world.bounds)
            .unwrap_or_else(PictureRect::max_rect);

        let map_local_to_surface = SpaceMapper::new(
            surface_spatial_node_index,
            pic_bounds,
        );

        SurfaceInfo {
            unclipped_local_rect: PictureRect::zero(),
            clipped_local_rect: PictureRect::zero(),
            is_opaque: false,
            clipping_rect: PictureRect::zero(),
            map_local_to_surface,
            raster_spatial_node_index,
            surface_spatial_node_index,
            device_pixel_scale,
            world_scale_factors,
            local_scale,
            allow_snapping,
        }
    }

    /// Clamps the blur radius depending on scale factors.
    pub fn clamp_blur_radius(
        &self,
        x_blur_radius: f32,
        y_blur_radius: f32,
    ) -> (f32, f32) {
        // Clamping must occur after scale factors are applied, but scale factors are not applied
        // until later on. To clamp the blur radius, we first apply the scale factors and then clamp
        // and finally revert the scale factors.

        let sx_blur_radius = x_blur_radius * self.local_scale.0;
        let sy_blur_radius = y_blur_radius * self.local_scale.1;

        let largest_scaled_blur_radius = f32::max(
            sx_blur_radius * self.world_scale_factors.0,
            sy_blur_radius * self.world_scale_factors.1,
        );

        if largest_scaled_blur_radius > MAX_BLUR_RADIUS {
            let sf = MAX_BLUR_RADIUS / largest_scaled_blur_radius;
            (x_blur_radius * sf, y_blur_radius * sf)
        } else {
            // Return the original blur radius to avoid any rounding errors
            (x_blur_radius, y_blur_radius)
        }
    }

    pub fn map_to_device_rect(
        &self,
        local_rect: &PictureRect,
        spatial_tree: &SpatialTree,
    ) -> DeviceRect {
        let raster_rect = if self.raster_spatial_node_index != self.surface_spatial_node_index {
            assert_eq!(self.device_pixel_scale.0, 1.0);

            let local_to_world = SpaceMapper::new_with_target(
                spatial_tree.root_reference_frame_index(),
                self.surface_spatial_node_index,
                WorldRect::max_rect(),
                spatial_tree,
            );

            local_to_world.map(&local_rect).unwrap()
        } else {
            local_rect.cast_unit()
        };

        raster_rect * self.device_pixel_scale
    }

    /// Clip and transform a local rect to a device rect suitable for allocating
    /// a child off-screen surface of this surface (e.g. for clip-masks)
    pub fn get_surface_rect(
        &self,
        local_rect: &PictureRect,
        spatial_tree: &SpatialTree,
    ) -> Option<DeviceRect> {
        let local_rect = match local_rect.intersection(&self.clipping_rect) {
            Some(rect) => rect,
            None => return None,
        };

        let raster_rect = if self.raster_spatial_node_index != self.surface_spatial_node_index {
            assert_eq!(self.device_pixel_scale.0, 1.0);

            let local_to_world = SpaceMapper::new_with_target(
                spatial_tree.root_reference_frame_index(),
                self.surface_spatial_node_index,
                WorldRect::max_rect(),
                spatial_tree,
            );

            local_to_world.map(&local_rect).unwrap()
        } else {
            local_rect.cast_unit()
        };

        Some((raster_rect * self.device_pixel_scale).round_out())
    }
}

/// Information from `get_surface_rects` about the allocated size, UV sampling
/// parameters etc for an off-screen surface
struct SurfaceAllocInfo {
    task_size: DeviceIntSize,
    needs_scissor_rect: bool,
    clipped: DeviceRect,
    unclipped: DeviceRect,
    clipped_local: PictureRect,
    uv_rect_kind: UvRectKind,
}

#[derive(Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct RasterConfig {
    /// How this picture should be composited into
    /// the parent surface.
    // TODO(gw): We should remove this and just use what is in PicturePrimitive
    pub composite_mode: PictureCompositeMode,
    /// Index to the surface descriptor for this
    /// picture.
    pub surface_index: SurfaceIndex,
}

bitflags! {
    /// A set of flags describing why a picture may need a backing surface.
    #[cfg_attr(feature = "capture", derive(Serialize))]
    pub struct BlitReason: u32 {
        /// Mix-blend-mode on a child that requires isolation.
        const ISOLATE = 1;
        /// Clip node that _might_ require a surface.
        const CLIP = 2;
        /// Preserve-3D requires a surface for plane-splitting.
        const PRESERVE3D = 4;
        /// A backdrop that is reused which requires a surface.
        const BACKDROP = 8;
    }
}

/// Specifies how this Picture should be composited
/// onto the target it belongs to.
#[allow(dead_code)]
#[derive(Debug, Clone)]
#[cfg_attr(feature = "capture", derive(Serialize))]
pub enum PictureCompositeMode {
    /// Apply CSS mix-blend-mode effect.
    MixBlend(MixBlendMode),
    /// Apply a CSS filter (except component transfer).
    Filter(Filter),
    /// Apply a component transfer filter.
    ComponentTransferFilter(FilterDataHandle),
    /// Draw to intermediate surface, copy straight across. This
    /// is used for CSS isolation, and plane splitting.
    Blit(BlitReason),
    /// Used to cache a picture as a series of tiles.
    TileCache {
        slice_id: SliceId,
    },
    /// Apply an SVG filter
    SvgFilter(Vec<FilterPrimitive>, Vec<SFilterData>),
    /// A surface that is used as an input to another primitive
    IntermediateSurface,
}

impl PictureCompositeMode {
    pub fn get_rect(
        &self,
        surface: &SurfaceInfo,
        sub_rect: Option<LayoutRect>,
    ) -> LayoutRect {
        let surface_rect = match sub_rect {
            Some(sub_rect) => sub_rect,
            None => surface.clipped_local_rect.cast_unit(),
        };

        match self {
            PictureCompositeMode::Filter(Filter::Blur { width, height, should_inflate }) => {
                if *should_inflate {
                    let (width_factor, height_factor) = surface.clamp_blur_radius(*width, *height);

                    surface_rect.inflate(
                        width_factor.ceil() * BLUR_SAMPLE_SCALE,
                        height_factor.ceil() * BLUR_SAMPLE_SCALE,
                    )
                } else {
                    surface_rect
                }
            }
            PictureCompositeMode::Filter(Filter::DropShadows(ref shadows)) => {
                let mut max_blur_radius = 0.0;
                for shadow in shadows {
                    max_blur_radius = f32::max(max_blur_radius, shadow.blur_radius);
                }

                let (max_blur_radius_x, max_blur_radius_y) = surface.clamp_blur_radius(
                    max_blur_radius,
                    max_blur_radius,
                );
                let blur_inflation_x = max_blur_radius_x * BLUR_SAMPLE_SCALE;
                let blur_inflation_y = max_blur_radius_y * BLUR_SAMPLE_SCALE;

                surface_rect.inflate(blur_inflation_x, blur_inflation_y)
            }
            PictureCompositeMode::SvgFilter(primitives, _) => {
                let mut result_rect = surface_rect;
                let mut output_rects = Vec::with_capacity(primitives.len());

                for (cur_index, primitive) in primitives.iter().enumerate() {
                    let output_rect = match primitive.kind {
                        FilterPrimitiveKind::Blur(ref primitive) => {
                            let input = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            let width_factor = primitive.width.round() * BLUR_SAMPLE_SCALE;
                            let height_factor = primitive.height.round() * BLUR_SAMPLE_SCALE;
                            input.inflate(width_factor, height_factor)
                        }
                        FilterPrimitiveKind::DropShadow(ref primitive) => {
                            let inflation_factor = primitive.shadow.blur_radius.ceil() * BLUR_SAMPLE_SCALE;
                            let input = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            let shadow_rect = input.inflate(inflation_factor, inflation_factor);
                            input.union(&shadow_rect.translate(primitive.shadow.offset * Scale::new(1.0)))
                        }
                        FilterPrimitiveKind::Blend(ref primitive) => {
                            primitive.input1.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect)
                                .union(&primitive.input2.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect))
                        }
                        FilterPrimitiveKind::Composite(ref primitive) => {
                            primitive.input1.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect)
                                .union(&primitive.input2.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect))
                        }
                        FilterPrimitiveKind::Identity(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::Opacity(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::ColorMatrix(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::ComponentTransfer(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::Offset(ref primitive) => {
                            let input_rect = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            input_rect.translate(primitive.offset * Scale::new(1.0))
                        },

                        FilterPrimitiveKind::Flood(..) => surface_rect,
                    };
                    output_rects.push(output_rect);
                    result_rect = result_rect.union(&output_rect);
                }
                result_rect
            }
            _ => {
                surface_rect
            }
        }
    }

    pub fn get_coverage(
        &self,
        surface: &SurfaceInfo,
        sub_rect: Option<LayoutRect>,
    ) -> LayoutRect {
        let surface_rect = match sub_rect {
            Some(sub_rect) => sub_rect,
            None => surface.clipped_local_rect.cast_unit(),
        };

        match self {
            PictureCompositeMode::Filter(Filter::Blur { width, height, should_inflate }) => {
                if *should_inflate {
                    let (width_factor, height_factor) = surface.clamp_blur_radius(*width, *height);

                    surface_rect.inflate(
                        width_factor.ceil() * BLUR_SAMPLE_SCALE,
                        height_factor.ceil() * BLUR_SAMPLE_SCALE,
                    )
                } else {
                    surface_rect
                }
            }
            PictureCompositeMode::Filter(Filter::DropShadows(ref shadows)) => {
                let mut rect = surface_rect;

                for shadow in shadows {
                    let (blur_radius_x, blur_radius_y) = surface.clamp_blur_radius(
                        shadow.blur_radius,
                        shadow.blur_radius,
                    );
                    let blur_inflation_x = blur_radius_x * BLUR_SAMPLE_SCALE;
                    let blur_inflation_y = blur_radius_y * BLUR_SAMPLE_SCALE;

                    let shadow_rect = surface_rect
                        .translate(shadow.offset)
                        .inflate(blur_inflation_x, blur_inflation_y);
                    rect = rect.union(&shadow_rect);
                }

                rect
            }
            PictureCompositeMode::SvgFilter(primitives, _) => {
                let mut result_rect = surface_rect;
                let mut output_rects = Vec::with_capacity(primitives.len());

                for (cur_index, primitive) in primitives.iter().enumerate() {
                    let output_rect = match primitive.kind {
                        FilterPrimitiveKind::Blur(ref primitive) => {
                            let input = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            let width_factor = primitive.width.round() * BLUR_SAMPLE_SCALE;
                            let height_factor = primitive.height.round() * BLUR_SAMPLE_SCALE;

                            input.inflate(width_factor, height_factor)
                        }
                        FilterPrimitiveKind::DropShadow(ref primitive) => {
                            let inflation_factor = primitive.shadow.blur_radius.ceil() * BLUR_SAMPLE_SCALE;
                            let input = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            let shadow_rect = input.inflate(inflation_factor, inflation_factor);
                            input.union(&shadow_rect.translate(primitive.shadow.offset * Scale::new(1.0)))
                        }
                        FilterPrimitiveKind::Blend(ref primitive) => {
                            primitive.input1.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect)
                                .union(&primitive.input2.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect))
                        }
                        FilterPrimitiveKind::Composite(ref primitive) => {
                            primitive.input1.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect)
                                .union(&primitive.input2.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect))
                        }
                        FilterPrimitiveKind::Identity(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::Opacity(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::ColorMatrix(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::ComponentTransfer(ref primitive) =>
                            primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect),
                        FilterPrimitiveKind::Offset(ref primitive) => {
                            let input_rect = primitive.input.to_index(cur_index).map(|index| output_rects[index]).unwrap_or(surface_rect);
                            input_rect.translate(primitive.offset * Scale::new(1.0))
                        },

                        FilterPrimitiveKind::Flood(..) => surface_rect,
                    };
                    output_rects.push(output_rect);
                    result_rect = result_rect.union(&output_rect);
                }
                result_rect
            }
            _ => {
                surface_rect
            }
        }
    }
}

/// Enum value describing the place of a picture in a 3D context.
#[derive(Clone, Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
pub enum Picture3DContext<C> {
    /// The picture is not a part of 3D context sub-hierarchy.
    Out,
    /// The picture is a part of 3D context.
    In {
        /// Additional data per child for the case of this a root of 3D hierarchy.
        root_data: Option<Vec<C>>,
        /// The spatial node index of an "ancestor" element, i.e. one
        /// that establishes the transformed element's containing block.
        ///
        /// See CSS spec draft for more details:
        /// https://drafts.csswg.org/css-transforms-2/#accumulated-3d-transformation-matrix-computation
        ancestor_index: SpatialNodeIndex,
        /// Index in the built scene's array of plane splitters.
        plane_splitter_index: PlaneSplitterIndex,
    },
}

/// Information about a preserve-3D hierarchy child that has been plane-split
/// and ordered according to the view direction.
#[derive(Clone, Debug)]
#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct OrderedPictureChild {
    pub anchor: PlaneSplitAnchor,
    pub gpu_address: GpuCacheAddress,
}

bitflags! {
    /// A set of flags describing why a picture may need a backing surface.
    #[cfg_attr(feature = "capture", derive(Serialize))]
    pub struct ClusterFlags: u32 {
        /// Whether this cluster is visible when the position node is a backface.
        const IS_BACKFACE_VISIBLE = 1;
        /// This flag is set during the first pass picture traversal, depending on whether
        /// the cluster is visible or not. It's read during the second pass when primitives
        /// consult their owning clusters to see if the primitive itself is visible.
        const IS_VISIBLE = 2;
    }
}

/// Descriptor for a cluster of primitives. For now, this is quite basic but will be
/// extended to handle more spatial clustering of primitives.
#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct PrimitiveCluster {
    /// The positioning node for this cluster.
    pub spatial_node_index: SpatialNodeIndex,
    /// The bounding rect of the cluster, in the local space of the spatial node.
    /// This is used to quickly determine the overall bounding rect for a picture
    /// during the first picture traversal, which is needed for local scale
    /// determination, and render task size calculations.
    bounding_rect: LayoutRect,
    /// a part of the cluster that we know to be opaque if any. Does not always
    /// describe the entire opaque region, but all content within that rect must
    /// be opaque.
    pub opaque_rect: LayoutRect,
    /// The range of primitive instance indices associated with this cluster.
    pub prim_range: Range<usize>,
    /// Various flags / state for this cluster.
    pub flags: ClusterFlags,
}

impl PrimitiveCluster {
    /// Construct a new primitive cluster for a given positioning node.
    fn new(
        spatial_node_index: SpatialNodeIndex,
        flags: ClusterFlags,
        first_instance_index: usize,
    ) -> Self {
        PrimitiveCluster {
            bounding_rect: LayoutRect::zero(),
            opaque_rect: LayoutRect::zero(),
            spatial_node_index,
            flags,
            prim_range: first_instance_index..first_instance_index
        }
    }

    /// Return true if this cluster is compatible with the given params
    pub fn is_compatible(
        &self,
        spatial_node_index: SpatialNodeIndex,
        flags: ClusterFlags,
        instance_index: usize,
    ) -> bool {
        self.flags == flags &&
        self.spatial_node_index == spatial_node_index &&
        instance_index == self.prim_range.end
    }

    pub fn prim_range(&self) -> Range<usize> {
        self.prim_range.clone()
    }

    /// Add a primitive instance to this cluster, at the start or end
    fn add_instance(
        &mut self,
        culling_rect: &LayoutRect,
        instance_index: usize,
    ) {
        debug_assert_eq!(instance_index, self.prim_range.end);
        self.bounding_rect = self.bounding_rect.union(culling_rect);
        self.prim_range.end += 1;
    }
}

/// A list of primitive instances that are added to a picture
/// This ensures we can keep a list of primitives that
/// are pictures, for a fast initial traversal of the picture
/// tree without walking the instance list.
#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct PrimitiveList {
    /// List of primitives grouped into clusters.
    pub clusters: Vec<PrimitiveCluster>,
    pub child_pictures: Vec<PictureIndex>,
    /// The number of preferred compositor surfaces that were found when
    /// adding prims to this list.
    pub compositor_surface_count: usize,
}

impl PrimitiveList {
    /// Construct an empty primitive list. This is
    /// just used during the take_context / restore_context
    /// borrow check dance, which will be removed as the
    /// picture traversal pass is completed.
    pub fn empty() -> Self {
        PrimitiveList {
            clusters: Vec::new(),
            child_pictures: Vec::new(),
            compositor_surface_count: 0,
        }
    }

    pub fn merge(&mut self, other: PrimitiveList) {
        self.clusters.extend(other.clusters);
        self.child_pictures.extend(other.child_pictures);
        self.compositor_surface_count += other.compositor_surface_count;
    }

    /// Add a primitive instance to the end of the list
    pub fn add_prim(
        &mut self,
        prim_instance: PrimitiveInstance,
        prim_rect: LayoutRect,
        spatial_node_index: SpatialNodeIndex,
        prim_flags: PrimitiveFlags,
        prim_instances: &mut Vec<PrimitiveInstance>,
        clip_tree_builder: &ClipTreeBuilder,
    ) {
        let mut flags = ClusterFlags::empty();

        // Pictures are always put into a new cluster, to make it faster to
        // iterate all pictures in a given primitive list.
        match prim_instance.kind {
            PrimitiveInstanceKind::Picture { pic_index, .. } => {
                self.child_pictures.push(pic_index);
            }
            _ => {}
        }

        if prim_flags.contains(PrimitiveFlags::IS_BACKFACE_VISIBLE) {
            flags.insert(ClusterFlags::IS_BACKFACE_VISIBLE);
        }

        if prim_flags.contains(PrimitiveFlags::PREFER_COMPOSITOR_SURFACE) {
            self.compositor_surface_count += 1;
        }

        let clip_leaf = clip_tree_builder.get_leaf(prim_instance.clip_leaf_id);
        let culling_rect = clip_leaf.local_clip_rect
            .intersection(&prim_rect)
            .unwrap_or_else(LayoutRect::zero);

        let instance_index = prim_instances.len();
        prim_instances.push(prim_instance);

        if let Some(cluster) = self.clusters.last_mut() {
            if cluster.is_compatible(spatial_node_index, flags, instance_index) {
                cluster.add_instance(&culling_rect, instance_index);
                return;
            }
        }

        // Same idea with clusters, using a different distribution.
        let clusters_len = self.clusters.len();
        if clusters_len == self.clusters.capacity() {
            let next_alloc = match clusters_len {
                1 ..= 15 => 16 - clusters_len,
                16 ..= 127 => 128 - clusters_len,
                _ => clusters_len * 2,
            };

            self.clusters.reserve(next_alloc);
        }

        let mut cluster = PrimitiveCluster::new(
            spatial_node_index,
            flags,
            instance_index,
        );

        cluster.add_instance(&culling_rect, instance_index);
        self.clusters.push(cluster);
    }

    /// Returns true if there are no clusters (and thus primitives)
    pub fn is_empty(&self) -> bool {
        self.clusters.is_empty()
    }
}

bitflags! {
    #[cfg_attr(feature = "capture", derive(Serialize))]
    /// Flags describing properties for a given PicturePrimitive
    pub struct PictureFlags : u8 {
        /// This picture is a resolve target (doesn't actually render content itself,
        /// will have content copied in to it)
        const IS_RESOLVE_TARGET = 1 << 0;
        /// This picture establishes a sub-graph, which affects how SurfaceBuilder will
        /// set up dependencies in the render task graph
        const IS_SUB_GRAPH = 1 << 1;
        /// If set, this picture should not apply snapping via changing the raster root
        const DISABLE_SNAPPING = 1 << 2;
    }
}

#[cfg_attr(feature = "capture", derive(Serialize))]
pub struct PicturePrimitive {
    /// List of primitives, and associated info for this picture.
    pub prim_list: PrimitiveList,

    /// If false and transform ends up showing the back of the picture,
    /// it will be considered invisible.
    pub is_backface_visible: bool,

    pub primary_render_task_id: Option<RenderTaskId>,
    /// If a mix-blend-mode, contains the render task for
    /// the readback of the framebuffer that we use to sample
    /// from in the mix-blend-mode shader.
    /// For drop-shadow filter, this will store the original
    /// picture task which would be rendered on screen after
    /// blur pass.
    pub secondary_render_task_id: Option<RenderTaskId>,
    /// How this picture should be composited.
    /// If None, don't composite - just draw directly on parent surface.
    pub composite_mode: Option<PictureCompositeMode>,

    pub raster_config: Option<RasterConfig>,
    pub context_3d: Picture3DContext<OrderedPictureChild>,

    // Optional cache handles for storing extra data
    // in the GPU cache, depending on the type of
    // picture.
    pub extra_gpu_data_handles: SmallVec<[GpuCacheHandle; 1]>,

    /// The spatial node index of this picture when it is
    /// composited into the parent picture.
    pub spatial_node_index: SpatialNodeIndex,

    /// Store the state of the previous local rect
    /// for this picture. We need this in order to know when
    /// to invalidate segments / drop-shadow gpu cache handles.
    pub prev_local_rect: LayoutRect,

    /// If false, this picture needs to (re)build segments
    /// if it supports segment rendering. This can occur
    /// if the local rect of the picture changes due to
    /// transform animation and/or scrolling.
    pub segments_are_valid: bool,

    /// Set to true if we know for sure the picture is fully opaque.
    pub is_opaque: bool,

    /// Requested raster space for this picture
    pub raster_space: RasterSpace,

    /// Flags for this picture primitive
    pub flags: PictureFlags,
}

impl PicturePrimitive {
    pub fn print<T: PrintTreePrinter>(
        &self,
        pictures: &[Self],
        self_index: PictureIndex,
        pt: &mut T,
    ) {
        pt.new_level(format!("{:?}", self_index));
        pt.add_item(format!("cluster_count: {:?}", self.prim_list.clusters.len()));
        pt.add_item(format!("spatial_node_index: {:?}", self.spatial_node_index));
        pt.add_item(format!("raster_config: {:?}", self.raster_config));
        pt.add_item(format!("composite_mode: {:?}", self.composite_mode));
        pt.add_item(format!("flags: {:?}", self.flags));

        for child_pic_index in &self.prim_list.child_pictures {
            pictures[child_pic_index.0].print(pictures, *child_pic_index, pt);
        }

        pt.end_level();
    }

    /// Returns true if this picture supports segmented rendering.
    pub fn can_use_segments(&self) -> bool {
        match self.raster_config {
            // TODO(gw): Support brush segment rendering for filter and mix-blend
            //           shaders. It's possible this already works, but I'm just
            //           applying this optimization to Blit mode for now.
            Some(RasterConfig { composite_mode: PictureCompositeMode::MixBlend(..), .. }) |
            Some(RasterConfig { composite_mode: PictureCompositeMode::Filter(..), .. }) |
            Some(RasterConfig { composite_mode: PictureCompositeMode::ComponentTransferFilter(..), .. }) |
            Some(RasterConfig { composite_mode: PictureCompositeMode::TileCache { .. }, .. }) |
            Some(RasterConfig { composite_mode: PictureCompositeMode::SvgFilter(..), .. }) |
            Some(RasterConfig { composite_mode: PictureCompositeMode::IntermediateSurface, .. }) |
            None => {
                false
            }
            Some(RasterConfig { composite_mode: PictureCompositeMode::Blit(reason), ..}) => {
                reason == BlitReason::CLIP
            }
        }
    }

    fn resolve_scene_properties(&mut self, properties: &SceneProperties) {
        match self.composite_mode {
            Some(PictureCompositeMode::Filter(ref mut filter)) => {
                match *filter {
                    Filter::Opacity(ref binding, ref mut value) => {
                        *value = properties.resolve_float(binding);
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }

    pub fn is_visible(
        &self,
        spatial_tree: &SpatialTree,
    ) -> bool {
        if let Some(PictureCompositeMode::Filter(ref filter)) = self.composite_mode {
            if !filter.is_visible() {
                return false;
            }
        }

        // For out-of-preserve-3d pictures, the backface visibility is determined by
        // the local transform only.
        // Note: we aren't taking the transform relative to the parent picture,
        // since picture tree can be more dense than the corresponding spatial tree.
        if !self.is_backface_visible {
            if let Picture3DContext::Out = self.context_3d {
                match spatial_tree.get_local_visible_face(self.spatial_node_index) {
                    VisibleFace::Front => {}
                    VisibleFace::Back => return false,
                }
            }
        }

        true
    }

    pub fn new_image(
        composite_mode: Option<PictureCompositeMode>,
        context_3d: Picture3DContext<OrderedPictureChild>,
        prim_flags: PrimitiveFlags,
        prim_list: PrimitiveList,
        spatial_node_index: SpatialNodeIndex,
        raster_space: RasterSpace,
        flags: PictureFlags,
    ) -> Self {
        PicturePrimitive {
            prim_list,
            primary_render_task_id: None,
            secondary_render_task_id: None,
            composite_mode,
            raster_config: None,
            context_3d,
            extra_gpu_data_handles: SmallVec::new(),
            is_backface_visible: prim_flags.contains(PrimitiveFlags::IS_BACKFACE_VISIBLE),
            spatial_node_index,
            prev_local_rect: LayoutRect::zero(),
            segments_are_valid: false,
            is_opaque: false,
            raster_space,
            flags,
        }
    }

    pub fn take_context(
        &mut self,
        pic_index: PictureIndex,
        parent_surface_index: Option<SurfaceIndex>,
        parent_subpixel_mode: SubpixelMode,
        frame_state: &mut FrameBuildingState,
        frame_context: &FrameBuildingContext,
        scratch: &mut PrimitiveScratchBuffer,
        tile_caches: &mut FastHashMap<SliceId, Box<TileCacheInstance>>,
    ) -> Option<(PictureContext, PictureState, PrimitiveList)> {
        self.primary_render_task_id = None;
        self.secondary_render_task_id = None;

        if !self.is_visible(frame_context.spatial_tree) {
            return None;
        }

        profile_scope!("take_context");

        let surface_index = match self.raster_config {
            Some(ref raster_config) => raster_config.surface_index,
            None => parent_surface_index.expect("bug: no parent"),
        };
        let surface_spatial_node_index = frame_state.surfaces[surface_index.0].surface_spatial_node_index;

        let map_pic_to_world = SpaceMapper::new_with_target(
            frame_context.root_spatial_node_index,
            surface_spatial_node_index,
            frame_context.global_screen_world_rect,
            frame_context.spatial_tree,
        );

        let pic_bounds = map_pic_to_world
            .unmap(&map_pic_to_world.bounds)
            .unwrap_or_else(PictureRect::max_rect);

        let map_local_to_pic = SpaceMapper::new(
            surface_spatial_node_index,
            pic_bounds,
        );

        match self.raster_config {
            Some(RasterConfig { surface_index, composite_mode: PictureCompositeMode::TileCache { slice_id }, .. }) => {
                let tile_cache = tile_caches.get_mut(&slice_id).unwrap();
                let mut debug_info = SliceDebugInfo::new();
                let mut surface_render_tasks = FastHashMap::default();
                let mut surface_local_dirty_rect = PictureRect::zero();
                let device_pixel_scale = frame_state
                    .surfaces[surface_index.0]
                    .device_pixel_scale;
                let mut at_least_one_tile_visible = false;

                // Get the overall world space rect of the picture cache. Used to clip
                // the tile rects below for occlusion testing to the relevant area.
                let world_clip_rect = map_pic_to_world
                    .map(&tile_cache.local_clip_rect)
                    .expect("bug: unable to map clip rect")
                    .round();
                let device_clip_rect = (world_clip_rect * frame_context.global_device_pixel_scale).round();

                for (sub_slice_index, sub_slice) in tile_cache.sub_slices.iter_mut().enumerate() {
                    for tile in sub_slice.tiles.values_mut() {
                        if tile.is_visible {
                            // Get the world space rect that this tile will actually occupy on screen
                            let world_draw_rect = world_clip_rect.intersection(&tile.world_valid_rect);

                            // If that draw rect is occluded by some set of tiles in front of it,
                            // then mark it as not visible and skip drawing. When it's not occluded
                            // it will fail this test, and get rasterized by the render task setup
                            // code below.
                            match world_draw_rect {
                                Some(world_draw_rect) => {
                                    // Only check for occlusion on visible tiles that are fixed position.
                                    if tile_cache.spatial_node_index == frame_context.root_spatial_node_index &&
                                       frame_state.composite_state.occluders.is_tile_occluded(tile.z_id, world_draw_rect) {
                                        // If this tile has an allocated native surface, free it, since it's completely
                                        // occluded. We will need to re-allocate this surface if it becomes visible,
                                        // but that's likely to be rare (e.g. when there is no content display list
                                        // for a frame or two during a tab switch).
                                        let surface = tile.surface.as_mut().expect("no tile surface set!");

                                        if let TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { id, .. }, .. } = surface {
                                            if let Some(id) = id.take() {
                                                frame_state.resource_cache.destroy_compositor_tile(id);
                                            }
                                        }

                                        tile.is_visible = false;

                                        if frame_context.fb_config.testing {
                                            debug_info.tiles.insert(
                                                tile.tile_offset,
                                                TileDebugInfo::Occluded,
                                            );
                                        }

                                        continue;
                                    }
                                }
                                None => {
                                    tile.is_visible = false;
                                }
                            }
                        }

                        // If we get here, we want to ensure that the surface remains valid in the texture
                        // cache, _even if_ it's not visible due to clipping or being scrolled off-screen.
                        // This ensures that we retain valid tiles that are off-screen, but still in the
                        // display port of this tile cache instance.
                        if let Some(TileSurface::Texture { descriptor, .. }) = tile.surface.as_ref() {
                            if let SurfaceTextureDescriptor::TextureCache { handle: Some(handle), .. } = descriptor {
                                frame_state.resource_cache
                                    .picture_textures.request(handle, frame_state.gpu_cache);
                            }
                        }

                        // If the tile has been found to be off-screen / clipped, skip any further processing.
                        if !tile.is_visible {
                            if frame_context.fb_config.testing {
                                debug_info.tiles.insert(
                                    tile.tile_offset,
                                    TileDebugInfo::Culled,
                                );
                            }

                            continue;
                        }

                        at_least_one_tile_visible = true;

                        if frame_context.debug_flags.contains(DebugFlags::PICTURE_CACHING_DBG) {
                            tile.root.draw_debug_rects(
                                &map_pic_to_world,
                                tile.is_opaque,
                                tile.current_descriptor.local_valid_rect,
                                scratch,
                                frame_context.global_device_pixel_scale,
                            );

                            let label_offset = DeviceVector2D::new(
                                20.0 + sub_slice_index as f32 * 20.0,
                                30.0 + sub_slice_index as f32 * 20.0,
                            );
                            let tile_device_rect = tile.world_tile_rect * frame_context.global_device_pixel_scale;
                            if tile_device_rect.height() >= label_offset.y {
                                let surface = tile.surface.as_ref().expect("no tile surface set!");

                                scratch.push_debug_string(
                                    tile_device_rect.min + label_offset,
                                    debug_colors::RED,
                                    format!("{:?}: s={} is_opaque={} surface={} sub={}",
                                            tile.id,
                                            tile_cache.slice,
                                            tile.is_opaque,
                                            surface.kind(),
                                            sub_slice_index,
                                    ),
                                );
                            }
                        }

                        if let TileSurface::Texture { descriptor, .. } = tile.surface.as_mut().unwrap() {
                            match descriptor {
                                SurfaceTextureDescriptor::TextureCache { ref handle, .. } => {
                                    let exists = handle.as_ref().map_or(false,
                                        |handle| frame_state.resource_cache.picture_textures.entry_exists(handle)
                                    );
                                    // Invalidate if the backing texture was evicted.
                                    if exists {
                                        // Request the backing texture so it won't get evicted this frame.
                                        // We specifically want to mark the tile texture as used, even
                                        // if it's detected not visible below and skipped. This is because
                                        // we maintain the set of tiles we care about based on visibility
                                        // during pre_update. If a tile still exists after that, we are
                                        // assuming that it's either visible or we want to retain it for
                                        // a while in case it gets scrolled back onto screen soon.
                                        // TODO(gw): Consider switching to manual eviction policy?
                                        frame_state.resource_cache
                                            .picture_textures
                                            .request(handle.as_ref().unwrap(), frame_state.gpu_cache);
                                    } else {
                                        // If the texture was evicted on a previous frame, we need to assume
                                        // that the entire tile rect is dirty.
                                        tile.invalidate(None, InvalidationReason::NoTexture);
                                    }
                                }
                                SurfaceTextureDescriptor::Native { id, .. } => {
                                    if id.is_none() {
                                        // There is no current surface allocation, so ensure the entire tile is invalidated
                                        tile.invalidate(None, InvalidationReason::NoSurface);
                                    }
                                }
                            }
                        }

                        // Ensure that the dirty rect doesn't extend outside the local valid rect.
                        tile.local_dirty_rect = tile.local_dirty_rect
                            .intersection(&tile.current_descriptor.local_valid_rect)
                            .unwrap_or_else(|| { tile.is_valid = true; PictureRect::zero() });

                        surface_local_dirty_rect = surface_local_dirty_rect.union(&tile.local_dirty_rect);

                        // Update the world/device dirty rect
                        let world_dirty_rect = map_pic_to_world.map(&tile.local_dirty_rect).expect("bug");

                        let device_rect = (tile.world_tile_rect * frame_context.global_device_pixel_scale).round();
                        tile.device_dirty_rect = (world_dirty_rect * frame_context.global_device_pixel_scale)
                            .round_out()
                            .intersection(&device_rect)
                            .unwrap_or_else(DeviceRect::zero);

                        if tile.is_valid {
                            if frame_context.fb_config.testing {
                                debug_info.tiles.insert(
                                    tile.tile_offset,
                                    TileDebugInfo::Valid,
                                );
                            }
                        } else {
                            // Add this dirty rect to the dirty region tracker. This must be done outside the if statement below,
                            // so that we include in the dirty region tiles that are handled by a background color only (no
                            // surface allocation).
                            tile_cache.dirty_region.add_dirty_region(
                                tile.local_dirty_rect,
                                frame_context.spatial_tree,
                            );

                            // Ensure that this texture is allocated.
                            if let TileSurface::Texture { ref mut descriptor } = tile.surface.as_mut().unwrap() {
                                match descriptor {
                                    SurfaceTextureDescriptor::TextureCache { ref mut handle } => {

                                        frame_state.resource_cache.picture_textures.update(
                                            tile_cache.current_tile_size,
                                            handle,
                                            frame_state.gpu_cache,
                                            &mut frame_state.resource_cache.texture_cache.next_id,
                                            &mut frame_state.resource_cache.texture_cache.pending_updates,
                                        );
                                    }
                                    SurfaceTextureDescriptor::Native { id } => {
                                        if id.is_none() {
                                            // Allocate a native surface id if we're in native compositing mode,
                                            // and we don't have a surface yet (due to first frame, or destruction
                                            // due to tile size changing etc).
                                            if sub_slice.native_surface.is_none() {
                                                let opaque = frame_state
                                                    .resource_cache
                                                    .create_compositor_surface(
                                                        tile_cache.virtual_offset,
                                                        tile_cache.current_tile_size,
                                                        true,
                                                    );

                                                let alpha = frame_state
                                                    .resource_cache
                                                    .create_compositor_surface(
                                                        tile_cache.virtual_offset,
                                                        tile_cache.current_tile_size,
                                                        false,
                                                    );

                                                sub_slice.native_surface = Some(NativeSurface {
                                                    opaque,
                                                    alpha,
                                                });
                                            }

                                            // Create the tile identifier and allocate it.
                                            let surface_id = if tile.is_opaque {
                                                sub_slice.native_surface.as_ref().unwrap().opaque
                                            } else {
                                                sub_slice.native_surface.as_ref().unwrap().alpha
                                            };

                                            let tile_id = NativeTileId {
                                                surface_id,
                                                x: tile.tile_offset.x,
                                                y: tile.tile_offset.y,
                                            };

                                            frame_state.resource_cache.create_compositor_tile(tile_id);

                                            *id = Some(tile_id);
                                        }
                                    }
                                }

                                // The cast_unit() here is because the `content_origin` is expected to be in
                                // device pixels, however we're establishing raster roots for picture cache
                                // tiles meaning the `content_origin` needs to be in the local space of that root.
                                // TODO(gw): `content_origin` should actually be in RasterPixels to be consistent
                                //           with both local / screen raster modes, but this involves a lot of
                                //           changes to render task and picture code.
                                let content_origin_f = tile.local_tile_rect.min.cast_unit() * device_pixel_scale;
                                let content_origin = content_origin_f.round();
                                // TODO: these asserts used to have a threshold of 0.01 but failed intermittently the
                                // gfx/layers/apz/test/mochitest/test_group_double_tap_zoom-2.html test on android.
                                // moving the rectangles in space mapping conversion code to the Box2D representaton
                                // made the failure happen more often.
                                debug_assert!((content_origin_f.x - content_origin.x).abs() < 0.15);
                                debug_assert!((content_origin_f.y - content_origin.y).abs() < 0.15);

                                let surface = descriptor.resolve(
                                    frame_state.resource_cache,
                                    tile_cache.current_tile_size,
                                );

                                let scissor_rect = frame_state.composite_state.get_surface_rect(
                                    &tile.local_dirty_rect,
                                    &tile.local_tile_rect,
                                    tile_cache.transform_index,
                                ).to_i32();

                                let valid_rect = frame_state.composite_state.get_surface_rect(
                                    &tile.current_descriptor.local_valid_rect,
                                    &tile.local_tile_rect,
                                    tile_cache.transform_index,
                                ).to_i32();

                                let composite_task_size = tile_cache.current_tile_size;

                                let tile_key = TileKey {
                                    sub_slice_index: SubSliceIndex::new(sub_slice_index),
                                    tile_offset: tile.tile_offset,
                                };

                                let mut clear_color = ColorF::TRANSPARENT;

                                if SubSliceIndex::new(sub_slice_index).is_primary() {
                                    if let Some(background_color) = tile_cache.background_color {
                                        clear_color = background_color;
                                    }

                                    // If this picture cache has a spanning_opaque_color, we will use
                                    // that as the clear color. The primitive that was detected as a
                                    // spanning primitive will have been set with IS_BACKDROP, causing
                                    // it to be skipped and removing everything added prior to it
                                    // during batching.
                                    if let Some(color) = tile_cache.backdrop.spanning_opaque_color {
                                        clear_color = color;
                                    }
                                }

                                let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                                // TODO(gw): As a performance optimization, we could skip the resolve picture
                                //           if the dirty rect is the same as the resolve rect (probably quite
                                //           common for effects that scroll underneath a backdrop-filter, for example).
                                let use_tile_composite = !tile.sub_graphs.is_empty();

                                if use_tile_composite {
                                    let mut local_content_rect = tile.local_dirty_rect;

                                    for (sub_graph_rect, surface_stack) in &tile.sub_graphs {
                                        if let Some(dirty_sub_graph_rect) = sub_graph_rect.intersection(&tile.local_dirty_rect) {
                                            for (composite_mode, surface_index) in surface_stack {
                                                let surface = &frame_state.surfaces[surface_index.0];

                                                let rect = composite_mode.get_coverage(
                                                    surface,
                                                    Some(dirty_sub_graph_rect.cast_unit()),
                                                ).cast_unit();

                                                local_content_rect = local_content_rect.union(&rect);
                                            }
                                        }
                                    }

                                    // We know that we'll never need to sample > 300 device pixels outside the tile
                                    // for blurring, so clamp the content rect here so that we don't try to allocate
                                    // a really large surface in the case of a drop-shadow with large offset.
                                    let max_content_rect = (tile.local_dirty_rect.cast_unit() * device_pixel_scale)
                                        .inflate(
                                            MAX_BLUR_RADIUS * BLUR_SAMPLE_SCALE,
                                            MAX_BLUR_RADIUS * BLUR_SAMPLE_SCALE,
                                        )
                                        .round_out()
                                        .to_i32();

                                    let content_device_rect = (local_content_rect.cast_unit() * device_pixel_scale)
                                        .round_out()
                                        .to_i32();

                                    let content_device_rect = content_device_rect
                                        .intersection(&max_content_rect)
                                        .expect("bug: no intersection with tile dirty rect");

                                    let content_task_size = content_device_rect.size();
                                    let normalized_content_rect = content_task_size.into();

                                    let inner_offset = content_origin + scissor_rect.min.to_vector().to_f32();
                                    let outer_offset = content_device_rect.min.to_f32();
                                    let sub_rect_offset = (inner_offset - outer_offset).round().to_i32();

                                    let render_task_id = frame_state.rg_builder.add().init(
                                        RenderTask::new_dynamic(
                                            content_task_size,
                                            RenderTaskKind::new_picture(
                                                content_task_size,
                                                true,
                                                content_device_rect.min.to_f32(),
                                                surface_spatial_node_index,
                                                // raster == surface implicitly for picture cache tiles
                                                surface_spatial_node_index,
                                                device_pixel_scale,
                                                Some(normalized_content_rect),
                                                None,
                                                Some(clear_color),
                                                cmd_buffer_index,
                                                false,
                                            )
                                        ),
                                    );

                                    let composite_task_id = frame_state.rg_builder.add().init(
                                        RenderTask::new(
                                            RenderTaskLocation::Static {
                                                surface: StaticRenderTaskSurface::PictureCache {
                                                    surface,
                                                },
                                                rect: composite_task_size.into(),
                                            },
                                            RenderTaskKind::new_tile_composite(
                                                sub_rect_offset,
                                                scissor_rect,
                                                valid_rect,
                                                clear_color,
                                            ),
                                        ),
                                    );

                                    surface_render_tasks.insert(
                                        tile_key,
                                        SurfaceTileDescriptor {
                                            current_task_id: render_task_id,
                                            composite_task_id: Some(composite_task_id),
                                            dirty_rect: tile.local_dirty_rect,
                                        },
                                    );
                                } else {
                                    let render_task_id = frame_state.rg_builder.add().init(
                                        RenderTask::new(
                                            RenderTaskLocation::Static {
                                                surface: StaticRenderTaskSurface::PictureCache {
                                                    surface,
                                                },
                                                rect: composite_task_size.into(),
                                            },
                                            RenderTaskKind::new_picture(
                                                composite_task_size,
                                                true,
                                                content_origin,
                                                surface_spatial_node_index,
                                                // raster == surface implicitly for picture cache tiles
                                                surface_spatial_node_index,
                                                device_pixel_scale,
                                                Some(scissor_rect),
                                                Some(valid_rect),
                                                Some(clear_color),
                                                cmd_buffer_index,
                                                false,
                                            )
                                        ),
                                    );

                                    surface_render_tasks.insert(
                                        tile_key,
                                        SurfaceTileDescriptor {
                                            current_task_id: render_task_id,
                                            composite_task_id: None,
                                            dirty_rect: tile.local_dirty_rect,
                                        },
                                    );
                                }
                            }

                            if frame_context.fb_config.testing {
                                debug_info.tiles.insert(
                                    tile.tile_offset,
                                    TileDebugInfo::Dirty(DirtyTileDebugInfo {
                                        local_valid_rect: tile.current_descriptor.local_valid_rect,
                                        local_dirty_rect: tile.local_dirty_rect,
                                    }),
                                );
                            }
                        }

                        let surface = tile.surface.as_ref().expect("no tile surface set!");

                        let descriptor = CompositeTileDescriptor {
                            surface_kind: surface.into(),
                            tile_id: tile.id,
                        };

                        let (surface, is_opaque) = match surface {
                            TileSurface::Color { color } => {
                                (CompositeTileSurface::Color { color: *color }, true)
                            }
                            TileSurface::Clear => {
                                // Clear tiles are rendered with blend mode pre-multiply-dest-out.
                                (CompositeTileSurface::Clear, false)
                            }
                            TileSurface::Texture { descriptor, .. } => {
                                let surface = descriptor.resolve(frame_state.resource_cache, tile_cache.current_tile_size);
                                (
                                    CompositeTileSurface::Texture { surface },
                                    tile.is_opaque
                                )
                            }
                        };

                        if is_opaque {
                            sub_slice.opaque_tile_descriptors.push(descriptor);
                        } else {
                            sub_slice.alpha_tile_descriptors.push(descriptor);
                        }

                        let composite_tile = CompositeTile {
                            kind: tile_kind(&surface, is_opaque),
                            surface,
                            local_rect: tile.local_tile_rect,
                            local_valid_rect: tile.current_descriptor.local_valid_rect,
                            local_dirty_rect: tile.local_dirty_rect,
                            device_clip_rect,
                            z_id: tile.z_id,
                            transform_index: tile_cache.transform_index,
                        };

                        sub_slice.composite_tiles.push(composite_tile);

                        // Now that the tile is valid, reset the dirty rect.
                        tile.local_dirty_rect = PictureRect::zero();
                        tile.is_valid = true;
                    }

                    // Sort the tile descriptor lists, since iterating values in the tile_cache.tiles
                    // hashmap doesn't provide any ordering guarantees, but we want to detect the
                    // composite descriptor as equal if the tiles list is the same, regardless of
                    // ordering.
                    sub_slice.opaque_tile_descriptors.sort_by_key(|desc| desc.tile_id);
                    sub_slice.alpha_tile_descriptors.sort_by_key(|desc| desc.tile_id);
                }

                // Check to see if we should add backdrops as native surfaces.
                let backdrop_rect = tile_cache.backdrop.backdrop_rect
                    .intersection(&tile_cache.local_rect)
                    .and_then(|r| {
                        r.intersection(&tile_cache.local_clip_rect)
                });

                let mut backdrop_in_use_and_visible = false;
                if let Some(backdrop_rect) = backdrop_rect {
                    let supports_surface_for_backdrop = match frame_state.composite_state.compositor_kind {
                        CompositorKind::Draw { .. } => {
                            false
                        }
                        CompositorKind::Native { capabilities, .. } => {
                            capabilities.supports_surface_for_backdrop
                        }
                    };
                    if supports_surface_for_backdrop && !tile_cache.found_prims_after_backdrop && at_least_one_tile_visible {
                        if let Some(BackdropKind::Color { color }) = tile_cache.backdrop.kind {
                            backdrop_in_use_and_visible = true;

                            // We're going to let the compositor handle the backdrop as a native surface.
                            // Hide all of our sub_slice tiles so they aren't also trying to draw it.
                            for sub_slice in &mut tile_cache.sub_slices {
                                for tile in sub_slice.tiles.values_mut() {
                                    tile.is_visible = false;
                                }
                            }

                            // Destroy our backdrop surface if it doesn't match the new color.
                            // TODO: This is a performance hit for animated color backdrops.
                            if let Some(backdrop_surface) = &tile_cache.backdrop_surface {
                                if backdrop_surface.color != color {
                                    frame_state.resource_cache.destroy_compositor_surface(backdrop_surface.id);
                                    tile_cache.backdrop_surface = None;
                                }
                            }

                            // Calculate the device_rect for the backdrop, which is just the backdrop_rect
                            // converted into world space and scaled to device pixels.
                            let world_backdrop_rect = map_pic_to_world.map(&backdrop_rect).expect("bug: unable to map backdrop rect");
                            let device_rect = (world_backdrop_rect * frame_context.global_device_pixel_scale).round();

                            // If we already have a backdrop surface, update the device rect. Otherwise, create
                            // a backdrop surface.
                            if let Some(backdrop_surface) = &mut tile_cache.backdrop_surface {
                                backdrop_surface.device_rect = device_rect;
                            } else {
                                // Create native compositor surface with color for the backdrop and store the id.
                                tile_cache.backdrop_surface = Some(BackdropSurface {
                                    id: frame_state.resource_cache.create_compositor_backdrop_surface(color),
                                    color,
                                    device_rect,
                                });
                            }
                        }
                    }
                }

                if !backdrop_in_use_and_visible {
                    if let Some(backdrop_surface) = &tile_cache.backdrop_surface {
                        // We've already allocated a backdrop surface, but we're not using it.
                        // Tell the compositor to get rid of it.
                        frame_state.resource_cache.destroy_compositor_surface(backdrop_surface.id);
                        tile_cache.backdrop_surface = None;
                    }
                }

                // If invalidation debugging is enabled, dump the picture cache state to a tree printer.
                if frame_context.debug_flags.contains(DebugFlags::INVALIDATION_DBG) {
                    tile_cache.print();
                }

                // If testing mode is enabled, write some information about the current state
                // of this picture cache (made available in RenderResults).
                if frame_context.fb_config.testing {
                    frame_state.composite_state
                        .picture_cache_debug
                        .slices
                        .insert(
                            tile_cache.slice,
                            debug_info,
                        );
                }

                let descriptor = SurfaceDescriptor::new_tiled(surface_render_tasks);

                frame_state.surface_builder.push_surface(
                    surface_index,
                    false,
                    surface_local_dirty_rect,
                    descriptor,
                    frame_state.surfaces,
                    frame_state.rg_builder,
                );
            }
            Some(ref mut raster_config) => {
                let pic_rect = frame_state
                    .surfaces[raster_config.surface_index.0]
                    .clipped_local_rect;

                let parent_surface_index = parent_surface_index.expect("bug: no parent for child surface");

                // Layout space for the picture is picture space from the
                // perspective of its child primitives.
                let local_rect = pic_rect * Scale::new(1.0);

                // If the precise rect changed since last frame, we need to invalidate
                // any segments and gpu cache handles for drop-shadows.
                // TODO(gw): Requiring storage of the `prev_precise_local_rect` here
                //           is a total hack. It's required because `prev_precise_local_rect`
                //           gets written to twice (during initial vis pass and also during
                //           prepare pass). The proper longer term fix for this is to make
                //           use of the conservative picture rect for segmenting (which should
                //           be done during scene building).
                if local_rect != self.prev_local_rect {
                    match raster_config.composite_mode {
                        PictureCompositeMode::Filter(Filter::DropShadows(..)) => {
                            for handle in &self.extra_gpu_data_handles {
                                frame_state.gpu_cache.invalidate(handle);
                            }
                        }
                        _ => {}
                    }
                    // Invalidate any segments built for this picture, since the local
                    // rect has changed.
                    self.segments_are_valid = false;
                    self.prev_local_rect = local_rect;
                }

                let max_surface_size = frame_context
                    .fb_config
                    .max_surface_override
                    .unwrap_or(MAX_SURFACE_SIZE) as f32;

                let surface_rects = match get_surface_rects(
                    raster_config.surface_index,
                    &raster_config.composite_mode,
                    parent_surface_index,
                    &mut frame_state.surfaces,
                    frame_context.spatial_tree,
                    max_surface_size,
                ) {
                    Some(rects) => rects,
                    None => return None,
                };

                let (raster_spatial_node_index, device_pixel_scale) = {
                    let surface = &frame_state.surfaces[surface_index.0];
                    (surface.raster_spatial_node_index, surface.device_pixel_scale)
                };
                let can_use_shared_surface = !self.flags.contains(PictureFlags::IS_RESOLVE_TARGET);

                let primary_render_task_id;
                let surface_descriptor;
                match raster_config.composite_mode {
                    PictureCompositeMode::TileCache { .. } => {
                        unreachable!("handled above");
                    }
                    PictureCompositeMode::Filter(Filter::Blur { width, height, .. }) => {
                        let surface = &frame_state.surfaces[raster_config.surface_index.0];
                        let (width, height) = surface.clamp_blur_radius(width, height);

                        let width_std_deviation = width * surface.local_scale.0 * device_pixel_scale.0;
                        let height_std_deviation = height * surface.local_scale.1 * device_pixel_scale.0;
                        let blur_std_deviation = DeviceSize::new(
                            width_std_deviation,
                            height_std_deviation,
                        );

                        let original_size = surface_rects.clipped.size();

                        // Adjust the size to avoid introducing sampling errors during the down-scaling passes.
                        // what would be even better is to rasterize the picture at the down-scaled size
                        // directly.
                        let adjusted_size = BlurTask::adjusted_blur_source_size(
                            original_size,
                            blur_std_deviation,
                        );

                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        // Since we (may have) adjusted the render task size for downscaling accuracy
                        // above, recalculate the uv rect for tasks that may sample from this blur output
                        let uv_rect_kind = calculate_uv_rect_kind(
                            DeviceRect::from_origin_and_size(surface_rects.clipped.min, adjusted_size.to_f32()),
                            surface_rects.unclipped,
                        );

                        let picture_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                adjusted_size,
                                RenderTaskKind::new_picture(
                                    adjusted_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(uv_rect_kind)
                        );

                        let blur_render_task_id = RenderTask::new_blur(
                            blur_std_deviation,
                            picture_task_id,
                            frame_state.rg_builder,
                            RenderTargetKind::Color,
                            None,
                            original_size.to_i32(),
                        );

                        primary_render_task_id = blur_render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_chained(
                            picture_task_id,
                            blur_render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::Filter(Filter::DropShadows(ref shadows)) => {
                        let surface = &frame_state.surfaces[raster_config.surface_index.0];

                        let device_rect = surface_rects.clipped;

                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let picture_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    device_rect.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                ),
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        let mut blur_tasks = BlurTaskCache::default();

                        self.extra_gpu_data_handles.resize(shadows.len(), GpuCacheHandle::new());

                        let mut blur_render_task_id = picture_task_id;
                        for shadow in shadows {
                            let (blur_radius_x, blur_radius_y) = surface.clamp_blur_radius(
                                shadow.blur_radius,
                                shadow.blur_radius,
                            );

                            blur_render_task_id = RenderTask::new_blur(
                                DeviceSize::new(
                                    blur_radius_x * surface.local_scale.0 * device_pixel_scale.0,
                                    blur_radius_y * surface.local_scale.1 * device_pixel_scale.0,
                                ),
                                picture_task_id,
                                frame_state.rg_builder,
                                RenderTargetKind::Color,
                                Some(&mut blur_tasks),
                                device_rect.size().to_i32(),
                            );
                        }

                        // Add this content picture as a dependency of the parent surface, to
                        // ensure it isn't free'd after the shadow uses it as an input.
                        frame_state.surface_builder.add_picture_render_task(picture_task_id);

                        primary_render_task_id = blur_render_task_id;
                        self.secondary_render_task_id = Some(picture_task_id);

                        surface_descriptor = SurfaceDescriptor::new_chained(
                            picture_task_id,
                            blur_render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::MixBlend(mode) if BlendMode::from_mix_blend_mode(
                        mode,
                        frame_context.fb_config.gpu_supports_advanced_blend,
                        frame_context.fb_config.advanced_blend_is_coherent,
                        frame_context.fb_config.dual_source_blending_is_supported,
                    ).is_none() => {
                        let parent_surface = &frame_state.surfaces[parent_surface_index.0];

                        // Create a space mapper that will allow mapping from the local rect
                        // of the mix-blend primitive into the space of the surface that we
                        // need to read back from. Note that we use the parent's raster spatial
                        // node here, so that we are in the correct device space of the parent
                        // surface, whether it establishes a raster root or not.
                        let map_pic_to_parent = SpaceMapper::new_with_target(
                            parent_surface.surface_spatial_node_index,
                            surface_spatial_node_index,
                            parent_surface.clipping_rect,
                            frame_context.spatial_tree,
                        );
                        let pic_in_raster_space = map_pic_to_parent
                            .map(&pic_rect)
                            .expect("bug: unable to map mix-blend content into parent");

                        // Apply device pixel ratio for parent surface to get into device
                        // pixels for that surface.
                        let backdrop_rect = pic_in_raster_space;
                        let parent_surface_rect = parent_surface.clipping_rect;

                        // If there is no available parent surface to read back from (for example, if
                        // the parent surface is affected by a clip that doesn't affect the child
                        // surface), then create a dummy 16x16 readback. In future, we could alter
                        // the composite mode of this primitive to skip the mix-blend, but for simplicity
                        // we just create a dummy readback for now.

                        let readback_task_id = match backdrop_rect.intersection(&parent_surface_rect) {
                            Some(available_rect) => {
                                // Calculate the UV coords necessary for the shader to sampler
                                // from the primitive rect within the readback region. This is
                                // 0..1 for aligned surfaces, but doing it this way allows
                                // accurate sampling if the primitive bounds have fractional values.

                                let backdrop_rect = parent_surface.map_to_device_rect(
                                    &backdrop_rect,
                                    frame_context.spatial_tree,
                                );

                                let available_rect = parent_surface.map_to_device_rect(
                                    &available_rect,
                                    frame_context.spatial_tree,
                                ).round_out();

                                let backdrop_uv = calculate_uv_rect_kind(
                                    available_rect,
                                    backdrop_rect,
                                );

                                frame_state.rg_builder.add().init(
                                    RenderTask::new_dynamic(
                                        available_rect.size().to_i32(),
                                        RenderTaskKind::new_readback(Some(available_rect.min)),
                                    ).with_uv_rect_kind(backdrop_uv)
                                )
                            }
                            None => {
                                frame_state.rg_builder.add().init(
                                    RenderTask::new_dynamic(
                                        DeviceIntSize::new(16, 16),
                                        RenderTaskKind::new_readback(None),
                                    )
                                )
                            }
                        };

                        frame_state.surface_builder.add_child_render_task(
                            readback_task_id,
                            frame_state.rg_builder,
                        );

                        self.secondary_render_task_id = Some(readback_task_id);

                        let task_size = surface_rects.clipped.size().to_i32();

                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let render_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                task_size,
                                RenderTaskKind::new_picture(
                                    task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        primary_render_task_id = render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_simple(
                            render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::Filter(..) => {
                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let render_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        primary_render_task_id = render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_simple(
                            render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::ComponentTransferFilter(..) => {
                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let render_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        primary_render_task_id = render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_simple(
                            render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::MixBlend(..) |
                    PictureCompositeMode::Blit(_) => {
                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let render_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        primary_render_task_id = render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_simple(
                            render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::IntermediateSurface => {
                        if !scratch.required_sub_graphs.contains(&pic_index) {
                            return None;
                        }

                        // TODO(gw): Remove all the mostly duplicated code in each of these
                        //           match cases (they used to be quite different).
                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let render_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        primary_render_task_id = render_task_id;

                        surface_descriptor = SurfaceDescriptor::new_simple(
                            render_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                    PictureCompositeMode::SvgFilter(ref primitives, ref filter_datas) => {
                        let cmd_buffer_index = frame_state.cmd_buffers.create_cmd_buffer();

                        let picture_task_id = frame_state.rg_builder.add().init(
                            RenderTask::new_dynamic(
                                surface_rects.task_size,
                                RenderTaskKind::new_picture(
                                    surface_rects.task_size,
                                    surface_rects.needs_scissor_rect,
                                    surface_rects.clipped.min,
                                    surface_spatial_node_index,
                                    raster_spatial_node_index,
                                    device_pixel_scale,
                                    None,
                                    None,
                                    None,
                                    cmd_buffer_index,
                                    can_use_shared_surface,
                                )
                            ).with_uv_rect_kind(surface_rects.uv_rect_kind)
                        );

                        let filter_task_id = RenderTask::new_svg_filter(
                            primitives,
                            filter_datas,
                            frame_state.rg_builder,
                            surface_rects.clipped.size().to_i32(),
                            surface_rects.uv_rect_kind,
                            picture_task_id,
                            device_pixel_scale,
                        );

                        primary_render_task_id = filter_task_id;

                        surface_descriptor = SurfaceDescriptor::new_chained(
                            picture_task_id,
                            filter_task_id,
                            surface_rects.clipped_local,
                        );
                    }
                }

                let is_sub_graph = self.flags.contains(PictureFlags::IS_SUB_GRAPH);

                frame_state.surface_builder.push_surface(
                    raster_config.surface_index,
                    is_sub_graph,
                    surface_rects.clipped_local,
                    surface_descriptor,
                    frame_state.surfaces,
                    frame_state.rg_builder,
                );

                self.primary_render_task_id = Some(primary_render_task_id);
            }
            None => {}
        };

        let state = PictureState {
            map_local_to_pic,
            map_pic_to_world,
        };

        let mut dirty_region_count = 0;

        // If this is a picture cache, push the dirty region to ensure any
        // child primitives are culled and clipped to the dirty rect(s).
        if let Some(RasterConfig { composite_mode: PictureCompositeMode::TileCache { slice_id }, .. }) = self.raster_config {
            let dirty_region = tile_caches[&slice_id].dirty_region.clone();
            frame_state.push_dirty_region(dirty_region);
            dirty_region_count += 1;
        }

        // Disallow subpixel AA if an intermediate surface is needed.
        // TODO(lsalzman): allow overriding parent if intermediate surface is opaque
        let subpixel_mode = match self.raster_config {
            Some(RasterConfig { ref composite_mode, .. }) => {
                let subpixel_mode = match composite_mode {
                    PictureCompositeMode::TileCache { slice_id } => {
                        tile_caches[&slice_id].subpixel_mode
                    }
                    PictureCompositeMode::Blit(..) |
                    PictureCompositeMode::ComponentTransferFilter(..) |
                    PictureCompositeMode::Filter(..) |
                    PictureCompositeMode::MixBlend(..) |
                    PictureCompositeMode::IntermediateSurface |
                    PictureCompositeMode::SvgFilter(..) => {
                        // TODO(gw): We can take advantage of the same logic that
                        //           exists in the opaque rect detection for tile
                        //           caches, to allow subpixel text on other surfaces
                        //           that can be detected as opaque.
                        SubpixelMode::Deny
                    }
                };

                subpixel_mode
            }
            None => {
                SubpixelMode::Allow
            }
        };

        // Still disable subpixel AA if parent forbids it
        let subpixel_mode = match (parent_subpixel_mode, subpixel_mode) {
            (SubpixelMode::Allow, SubpixelMode::Allow) => {
                // Both parent and this surface unconditionally allow subpixel AA
                SubpixelMode::Allow
            }
            (SubpixelMode::Allow, SubpixelMode::Conditional { allowed_rect }) => {
                // Parent allows, but we are conditional subpixel AA
                SubpixelMode::Conditional {
                    allowed_rect,
                }
            }
            (SubpixelMode::Conditional { allowed_rect }, SubpixelMode::Allow) => {
                // Propagate conditional subpixel mode to child pictures that allow subpixel AA
                SubpixelMode::Conditional {
                    allowed_rect,
                }
            }
            (SubpixelMode::Conditional { .. }, SubpixelMode::Conditional { ..}) => {
                unreachable!("bug: only top level picture caches have conditional subpixel");
            }
            (SubpixelMode::Deny, _) | (_, SubpixelMode::Deny) => {
                // Either parent or this surface explicitly deny subpixel, these take precedence
                SubpixelMode::Deny
            }
        };

        let context = PictureContext {
            pic_index,
            raster_spatial_node_index: frame_state.surfaces[surface_index.0].raster_spatial_node_index,
            surface_spatial_node_index,
            surface_index,
            dirty_region_count,
            subpixel_mode,
        };

        let prim_list = mem::replace(&mut self.prim_list, PrimitiveList::empty());

        Some((context, state, prim_list))
    }

    pub fn restore_context(
        &mut self,
        pic_index: PictureIndex,
        prim_list: PrimitiveList,
        context: PictureContext,
        prim_instances: &[PrimitiveInstance],
        frame_context: &FrameBuildingContext,
        frame_state: &mut FrameBuildingState,
    ) {
        // Pop any dirty regions this picture set
        for _ in 0 .. context.dirty_region_count {
            frame_state.pop_dirty_region();
        }

        if self.raster_config.is_some() {
            frame_state.surface_builder.pop_surface(
                pic_index,
                frame_state.rg_builder,
                frame_state.cmd_buffers,
            );
        }

        if let Picture3DContext::In { root_data: Some(ref mut list), plane_splitter_index, .. } = self.context_3d {
            let splitter = &mut frame_state.plane_splitters[plane_splitter_index.0];

            // Resolve split planes via BSP
            PicturePrimitive::resolve_split_planes(
                splitter,
                list,
                &mut frame_state.gpu_cache,
                &frame_context.spatial_tree,
            );

            // Add the child prims to the relevant command buffers
            let mut cmd_buffer_targets = Vec::new();
            for child in list {
                let child_prim_instance = &prim_instances[child.anchor.instance_index.0 as usize];

                if frame_state.surface_builder.get_cmd_buffer_targets_for_prim(
                    &child_prim_instance.vis,
                    &mut cmd_buffer_targets,
                ) {
                    let prim_cmd = PrimitiveCommand::complex(
                        child.anchor.instance_index,
                        child.gpu_address
                    );

                    frame_state.push_prim(
                        &prim_cmd,
                        child.anchor.spatial_node_index,
                        &cmd_buffer_targets,
                    );
                }
            }
        }

        self.prim_list = prim_list;
    }

    /// Add a primitive instance to the plane splitter. The function would generate
    /// an appropriate polygon, clip it against the frustum, and register with the
    /// given plane splitter.
    pub fn add_split_plane(
        splitter: &mut PlaneSplitter,
        spatial_tree: &SpatialTree,
        prim_spatial_node_index: SpatialNodeIndex,
        original_local_rect: LayoutRect,
        combined_local_clip_rect: &LayoutRect,
        world_rect: WorldRect,
        plane_split_anchor: PlaneSplitAnchor,
    ) -> bool {
        let transform = spatial_tree
            .get_world_transform(prim_spatial_node_index);
        let matrix = transform.clone().into_transform().cast().to_untyped();

        // Apply the local clip rect here, before splitting. This is
        // because the local clip rect can't be applied in the vertex
        // shader for split composites, since we are drawing polygons
        // rather that rectangles. The interpolation still works correctly
        // since we determine the UVs by doing a bilerp with a factor
        // from the original local rect.
        let local_rect = match original_local_rect
            .intersection(combined_local_clip_rect)
        {
            Some(rect) => rect.cast(),
            None => return false,
        };
        let world_rect = world_rect.cast();

        match transform {
            CoordinateSpaceMapping::Local => {
                let polygon = Polygon::from_rect(
                    local_rect.to_rect() * Scale::new(1.0),
                    plane_split_anchor,
                );
                splitter.add(polygon);
            }
            CoordinateSpaceMapping::ScaleOffset(scale_offset) if scale_offset.scale == Vector2D::new(1.0, 1.0) => {
                let inv_matrix = scale_offset.inverse().to_transform().cast();
                let polygon = Polygon::from_transformed_rect_with_inverse(
                    local_rect.to_rect().to_untyped(),
                    &matrix,
                    &inv_matrix,
                    plane_split_anchor,
                ).unwrap();
                splitter.add(polygon);
            }
            CoordinateSpaceMapping::ScaleOffset(_) |
            CoordinateSpaceMapping::Transform(_) => {
                let mut clipper = Clipper::new();
                let results = clipper.clip_transformed(
                    Polygon::from_rect(
                        local_rect.to_rect().to_untyped(),
                        plane_split_anchor,
                    ),
                    &matrix,
                    Some(world_rect.to_rect().to_untyped()),
                );
                if let Ok(results) = results {
                    for poly in results {
                        splitter.add(poly);
                    }
                }
            }
        }

        true
    }

    fn resolve_split_planes(
        splitter: &mut PlaneSplitter,
        ordered: &mut Vec<OrderedPictureChild>,
        gpu_cache: &mut GpuCache,
        spatial_tree: &SpatialTree,
    ) {
        ordered.clear();

        // Process the accumulated split planes and order them for rendering.
        // Z axis is directed at the screen, `sort` is ascending, and we need back-to-front order.
        let sorted = splitter.sort(vec3(0.0, 0.0, 1.0));
        ordered.reserve(sorted.len());
        for poly in sorted {
            let transform = match spatial_tree
                .get_world_transform(poly.anchor.spatial_node_index)
                .inverse()
            {
                Some(transform) => transform.into_transform(),
                // logging this would be a bit too verbose
                None => continue,
            };

            let local_points = [
                transform.transform_point3d(poly.points[0].cast_unit().to_f32()),
                transform.transform_point3d(poly.points[1].cast_unit().to_f32()),
                transform.transform_point3d(poly.points[2].cast_unit().to_f32()),
                transform.transform_point3d(poly.points[3].cast_unit().to_f32()),
            ];

            // If any of the points are un-transformable, just drop this
            // plane from drawing.
            if local_points.iter().any(|p| p.is_none()) {
                continue;
            }

            let p0 = local_points[0].unwrap();
            let p1 = local_points[1].unwrap();
            let p2 = local_points[2].unwrap();
            let p3 = local_points[3].unwrap();
            let gpu_blocks = [
                [p0.x, p0.y, p1.x, p1.y].into(),
                [p2.x, p2.y, p3.x, p3.y].into(),
            ];
            let gpu_handle = gpu_cache.push_per_frame_blocks(&gpu_blocks);
            let gpu_address = gpu_cache.get_address(&gpu_handle);

            ordered.push(OrderedPictureChild {
                anchor: poly.anchor,
                gpu_address,
            });
        }
    }

    /// Do initial checks to determine whether this picture should be drawn as part of the
    /// frame build.
    pub fn pre_update(
        &mut self,
        frame_context: &FrameBuildingContext,
    ) {
        // Resolve animation properties
        self.resolve_scene_properties(frame_context.scene_properties);
    }

    /// Called during initial picture traversal, before we know the
    /// bounding rect of children. It is possible to determine the
    /// surface / raster config now though.
    pub fn assign_surface(
        &mut self,
        frame_context: &FrameBuildingContext,
        parent_surface_index: Option<SurfaceIndex>,
        tile_caches: &mut FastHashMap<SliceId, Box<TileCacheInstance>>,
        surfaces: &mut Vec<SurfaceInfo>,
    ) -> Option<SurfaceIndex> {
        // Reset raster config in case we early out below.
        self.raster_config = None;

        match self.composite_mode {
            Some(ref composite_mode) => {
                let surface_spatial_node_index = self.spatial_node_index;

                // Currently, we ensure that the scaling factor is >= 1.0 as a smaller scale factor can result in blurry output.
                let mut min_scale;
                let mut max_scale = 1.0e32;

                // If a raster root is established, this surface should be scaled based on the scale factors of the surface raster to parent raster transform.
                // This scaling helps ensure that the content in this surface does not become blurry or pixelated when composited in the parent surface.

                let world_scale_factors = match parent_surface_index {
                    Some(parent_surface_index) => {
                        let parent_surface = &surfaces[parent_surface_index.0];

                        let local_to_surface = frame_context
                            .spatial_tree
                            .get_relative_transform(
                                surface_spatial_node_index,
                                parent_surface.surface_spatial_node_index,
                            );

                        // Since we can't determine reasonable scale factors for transforms
                        // with perspective, just use a scale of (1,1) for now, which is
                        // what Gecko does when it choosed to supplies a scale factor anyway.
                        // In future, we might be able to improve the quality here by taking
                        // into account the screen rect after clipping, but for now this gives
                        // better results than just taking the matrix scale factors.
                        let scale_factors = if local_to_surface.is_perspective() {
                            (1.0, 1.0)
                        } else {
                            local_to_surface.scale_factors()
                        };

                        let scale_factors = (
                            scale_factors.0 * parent_surface.world_scale_factors.0,
                            scale_factors.1 * parent_surface.world_scale_factors.1,
                        );

                        scale_factors
                    }
                    None => {
                        let local_to_surface_scale_factors = frame_context
                            .spatial_tree
                            .get_relative_transform(
                                surface_spatial_node_index,
                                frame_context.spatial_tree.root_reference_frame_index(),
                            )
                            .scale_factors();

                        let scale_factors = (
                            local_to_surface_scale_factors.0,
                            local_to_surface_scale_factors.1,
                        );

                        scale_factors
                    }
                };

                // TODO(gw): For now, we disable snapping on any sub-graph, as that implies
                //           that the spatial / raster node must be the same as the parent
                //           surface. In future, we may be able to support snapping in these
                //           cases (if it's even useful?) or perhaps add a ENABLE_SNAPPING
                //           picture flag, if the IS_SUB_GRAPH is ever useful in a different
                //           context.
                let allow_snapping = !self.flags.contains(PictureFlags::DISABLE_SNAPPING);

                // Check if there is perspective or if an SVG filter is applied, and thus whether a new
                // rasterization root should be established.
                let (device_pixel_scale, raster_spatial_node_index, local_scale, world_scale_factors) = match composite_mode {
                    PictureCompositeMode::TileCache { slice_id } => {
                        let tile_cache = tile_caches.get_mut(&slice_id).unwrap();

                        // We only update the raster scale if we're in high quality zoom mode, or there is no
                        // pinch-zoom active. This means that in low quality pinch-zoom, we retain the initial
                        // scale factor until the zoom ends, then select a high quality zoom factor for the next
                        // frame to be drawn.
                        let update_raster_scale =
                            !frame_context.fb_config.low_quality_pinch_zoom ||
                            !frame_context.spatial_tree.get_spatial_node(tile_cache.spatial_node_index).is_ancestor_or_self_zooming;

                        if update_raster_scale {
                            // Get the complete scale-offset from local space to device space
                            let local_to_device = get_relative_scale_offset(
                                tile_cache.spatial_node_index,
                                frame_context.root_spatial_node_index,
                                frame_context.spatial_tree,
                            );

                            tile_cache.current_raster_scale = local_to_device.scale.x;
                        }

                        // We may need to minify when zooming out picture cache tiles
                        min_scale = 0.0;

                        if frame_context.fb_config.low_quality_pinch_zoom {
                            // Force the scale for this tile cache to be the currently selected
                            // local raster scale, so we don't need to rasterize tiles during
                            // the pinch-zoom.
                            min_scale = tile_cache.current_raster_scale;
                            max_scale = tile_cache.current_raster_scale;
                        }

                        // Pick the largest scale factor of the transform for the scaling factor.
                        let scaling_factor = world_scale_factors.0.max(world_scale_factors.1).max(min_scale).min(max_scale);

                        let device_pixel_scale = Scale::new(scaling_factor);

                        (device_pixel_scale, surface_spatial_node_index, (1.0, 1.0), world_scale_factors)
                    }
                    _ => {
                        let surface_spatial_node = frame_context.spatial_tree.get_spatial_node(surface_spatial_node_index);

                        let enable_snapping =
                            allow_snapping &&
                            surface_spatial_node.coordinate_system_id == CoordinateSystemId::root() &&
                            surface_spatial_node.snapping_transform.is_some();

                        if enable_snapping {
                            let raster_spatial_node_index = frame_context.spatial_tree.root_reference_frame_index();

                            let local_to_raster_transform = frame_context
                                .spatial_tree
                                .get_relative_transform(
                                    self.spatial_node_index,
                                    raster_spatial_node_index,
                                );

                            let local_scale = local_to_raster_transform.scale_factors();

                            (Scale::new(1.0), raster_spatial_node_index, local_scale, (1.0, 1.0))
                        } else {
                            // If client supplied a specific local scale, use that instead of
                            // estimating from parent transform
                            let world_scale_factors = match self.raster_space {
                                RasterSpace::Screen => world_scale_factors,
                                RasterSpace::Local(scale) => (scale, scale),
                            };

                            let device_pixel_scale = Scale::new(world_scale_factors.0.max(world_scale_factors.1));

                            (device_pixel_scale, surface_spatial_node_index, (1.0, 1.0), world_scale_factors)
                        }
                    }
                };

                let surface = SurfaceInfo::new(
                    surface_spatial_node_index,
                    raster_spatial_node_index,
                    frame_context.global_screen_world_rect,
                    &frame_context.spatial_tree,
                    device_pixel_scale,
                    world_scale_factors,
                    local_scale,
                    allow_snapping,
                );

                let surface_index = SurfaceIndex(surfaces.len());
                surfaces.push(surface);

                self.raster_config = Some(RasterConfig {
                    composite_mode: composite_mode.clone(),
                    surface_index,
                });

                Some(surface_index)
            }
            None => {
                None
            }
        }
    }

    /// Called after updating child pictures during the initial
    /// picture traversal. Bounding rects are propagated from
    /// child pictures up to parent picture surfaces, so that the
    /// parent bounding rect includes any dynamic picture bounds.
    pub fn propagate_bounding_rect(
        &mut self,
        surface_index: SurfaceIndex,
        parent_surface_index: Option<SurfaceIndex>,
        surfaces: &mut [SurfaceInfo],
        frame_context: &FrameBuildingContext,
    ) {
        let surface = &mut surfaces[surface_index.0];

        for cluster in &mut self.prim_list.clusters {
            cluster.flags.remove(ClusterFlags::IS_VISIBLE);

            // Skip the cluster if backface culled.
            if !cluster.flags.contains(ClusterFlags::IS_BACKFACE_VISIBLE) {
                // For in-preserve-3d primitives and pictures, the backface visibility is
                // evaluated relative to the containing block.
                if let Picture3DContext::In { ancestor_index, .. } = self.context_3d {
                    let mut face = VisibleFace::Front;
                    frame_context.spatial_tree.get_relative_transform_with_face(
                        cluster.spatial_node_index,
                        ancestor_index,
                        Some(&mut face),
                    );
                    if face == VisibleFace::Back {
                        continue
                    }
                }
            }

            // No point including this cluster if it can't be transformed
            let spatial_node = &frame_context
                .spatial_tree
                .get_spatial_node(cluster.spatial_node_index);
            if !spatial_node.invertible {
                continue;
            }

            // Map the cluster bounding rect into the space of the surface, and
            // include it in the surface bounding rect.
            surface.map_local_to_surface.set_target_spatial_node(
                cluster.spatial_node_index,
                frame_context.spatial_tree,
            );

            // Mark the cluster visible, since it passed the invertible and
            // backface checks.
            cluster.flags.insert(ClusterFlags::IS_VISIBLE);
            if let Some(cluster_rect) = surface.map_local_to_surface.map(&cluster.bounding_rect) {
                surface.unclipped_local_rect = surface.unclipped_local_rect.union(&cluster_rect);
            }
        }

        // If this picture establishes a surface, then map the surface bounding
        // rect into the parent surface coordinate space, and propagate that up
        // to the parent.
        if let Some(ref mut raster_config) = self.raster_config {
            // Propagate up to parent surface, now that we know this surface's static rect
            if let Some(parent_surface_index) = parent_surface_index {
                let surface_rect = raster_config.composite_mode.get_coverage(
                    surface,
                    Some(surface.unclipped_local_rect.cast_unit()),
                );

                let parent_surface = &mut surfaces[parent_surface_index.0];
                parent_surface.map_local_to_surface.set_target_spatial_node(
                    self.spatial_node_index,
                    frame_context.spatial_tree,
                );

                // Drop shadows draw both a content and shadow rect, so need to expand the local
                // rect of any surfaces to be composited in parent surfaces correctly.

                if let Some(parent_surface_rect) = parent_surface
                    .map_local_to_surface
                    .map(&surface_rect)
                {
                    parent_surface.unclipped_local_rect =
                        parent_surface.unclipped_local_rect.union(&parent_surface_rect);
                }
            }
        }
    }

    pub fn prepare_for_render(
        &mut self,
        frame_state: &mut FrameBuildingState,
        data_stores: &mut DataStores,
    ) -> bool {
        let raster_config = match self.raster_config {
            Some(ref mut raster_config) => raster_config,
            None => {
                return true
            }
        };

        // TODO(gw): Almost all of the Picture types below use extra_gpu_cache_data
        //           to store the same type of data. The exception is the filter
        //           with a ColorMatrix, which stores the color matrix here. It's
        //           probably worth tidying this code up to be a bit more consistent.
        //           Perhaps store the color matrix after the common data, even though
        //           it's not used by that shader.

        match raster_config.composite_mode {
            PictureCompositeMode::TileCache { .. } => {}
            PictureCompositeMode::Filter(Filter::Blur { .. }) => {}
            PictureCompositeMode::Filter(Filter::DropShadows(ref shadows)) => {
                self.extra_gpu_data_handles.resize(shadows.len(), GpuCacheHandle::new());
                for (shadow, extra_handle) in shadows.iter().zip(self.extra_gpu_data_handles.iter_mut()) {
                    if let Some(mut request) = frame_state.gpu_cache.request(extra_handle) {
                        let surface = &frame_state.surfaces[raster_config.surface_index.0];
                        let prim_rect = surface.clipped_local_rect.cast_unit();

                        // Basic brush primitive header is (see end of prepare_prim_for_render_inner in prim_store.rs)
                        //  [brush specific data]
                        //  [segment_rect, segment data]
                        let (blur_inflation_x, blur_inflation_y) = surface.clamp_blur_radius(
                            shadow.blur_radius,
                            shadow.blur_radius,
                        );

                        let shadow_rect = prim_rect.inflate(
                            blur_inflation_x * BLUR_SAMPLE_SCALE,
                            blur_inflation_y * BLUR_SAMPLE_SCALE,
                        ).translate(shadow.offset);

                        // ImageBrush colors
                        request.push(shadow.color.premultiplied());
                        request.push(PremultipliedColorF::WHITE);
                        request.push([
                            shadow_rect.width(),
                            shadow_rect.height(),
                            0.0,
                            0.0,
                        ]);

                        // segment rect / extra data
                        request.push(shadow_rect);
                        request.push([0.0, 0.0, 0.0, 0.0]);
                    }
                }
            }
            PictureCompositeMode::Filter(ref filter) => {
                match *filter {
                    Filter::ColorMatrix(ref m) => {
                        if self.extra_gpu_data_handles.is_empty() {
                            self.extra_gpu_data_handles.push(GpuCacheHandle::new());
                        }
                        if let Some(mut request) = frame_state.gpu_cache.request(&mut self.extra_gpu_data_handles[0]) {
                            for i in 0..5 {
                                request.push([m[i*4], m[i*4+1], m[i*4+2], m[i*4+3]]);
                            }
                        }
                    }
                    Filter::Flood(ref color) => {
                        if self.extra_gpu_data_handles.is_empty() {
                            self.extra_gpu_data_handles.push(GpuCacheHandle::new());
                        }
                        if let Some(mut request) = frame_state.gpu_cache.request(&mut self.extra_gpu_data_handles[0]) {
                            request.push(color.to_array());
                        }
                    }
                    _ => {}
                }
            }
            PictureCompositeMode::ComponentTransferFilter(handle) => {
                let filter_data = &mut data_stores.filter_data[handle];
                filter_data.update(frame_state);
            }
            PictureCompositeMode::MixBlend(..) |
            PictureCompositeMode::Blit(_) |
            PictureCompositeMode::IntermediateSurface |
            PictureCompositeMode::SvgFilter(..) => {}
        }

        true
    }
}

fn get_transform_key(
    spatial_node_index: SpatialNodeIndex,
    cache_spatial_node_index: SpatialNodeIndex,
    spatial_tree: &SpatialTree,
) -> TransformKey {
    spatial_tree.get_relative_transform(
        spatial_node_index,
        cache_spatial_node_index,
    ).into()
}

/// A key for storing primitive comparison results during tile dependency tests.
#[derive(Debug, Copy, Clone, Eq, Hash, PartialEq)]
struct PrimitiveComparisonKey {
    prev_index: PrimitiveDependencyIndex,
    curr_index: PrimitiveDependencyIndex,
}

/// Information stored an image dependency
#[derive(Debug, Copy, Clone, PartialEq, PeekPoke, Default)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct ImageDependency {
    pub key: ImageKey,
    pub generation: ImageGeneration,
}

impl ImageDependency {
    pub const INVALID: ImageDependency = ImageDependency {
        key: ImageKey::DUMMY,
        generation: ImageGeneration::INVALID,
    };
}

/// In some cases, we need to know the dirty rect of all tiles in order
/// to correctly invalidate a primitive.
#[derive(Debug)]
struct DeferredDirtyTest {
    /// The tile rect that the primitive being checked affects
    tile_rect: TileRect,
    /// The picture-cache local rect of the primitive being checked
    prim_rect: PictureRect,
}

/// A helper struct to compare a primitive and all its sub-dependencies.
struct PrimitiveComparer<'a> {
    prev_data: &'a [u8],
    curr_data: &'a [u8],
    prev_frame_id: FrameId,
    curr_frame_id: FrameId,
    resource_cache: &'a ResourceCache,
    spatial_node_comparer: &'a mut SpatialNodeComparer,
    opacity_bindings: &'a FastHashMap<PropertyBindingId, OpacityBindingInfo>,
    color_bindings: &'a FastHashMap<PropertyBindingId, ColorBindingInfo>,
}

impl<'a> PrimitiveComparer<'a> {
    fn new(
        prev: &'a TileDescriptor,
        curr: &'a TileDescriptor,
        resource_cache: &'a ResourceCache,
        spatial_node_comparer: &'a mut SpatialNodeComparer,
        opacity_bindings: &'a FastHashMap<PropertyBindingId, OpacityBindingInfo>,
        color_bindings: &'a FastHashMap<PropertyBindingId, ColorBindingInfo>,
    ) -> Self {
        PrimitiveComparer {
            prev_data: &prev.dep_data,
            curr_data: &curr.dep_data,
            prev_frame_id: prev.last_updated_frame_id,
            curr_frame_id: curr.last_updated_frame_id,
            resource_cache,
            spatial_node_comparer,
            opacity_bindings,
            color_bindings,
        }
    }

    /// Check if two primitive descriptors are the same.
    fn compare_prim(
        &mut self,
        prev_desc: &PrimitiveDescriptor,
        curr_desc: &PrimitiveDescriptor,
    ) -> PrimitiveCompareResult {
        let resource_cache = self.resource_cache;
        let spatial_node_comparer = &mut self.spatial_node_comparer;
        let opacity_bindings = self.opacity_bindings;
        let color_bindings = self.color_bindings;

        // Check equality of the PrimitiveDescriptor
        if prev_desc != curr_desc {
            return PrimitiveCompareResult::Descriptor;
        }

        let mut prev_dep_data = &self.prev_data[prev_desc.dep_offset as usize ..];
        let mut curr_dep_data = &self.curr_data[curr_desc.dep_offset as usize ..];

        let mut prev_dep = PrimitiveDependency::SpatialNode { index: SpatialNodeIndex::INVALID };
        let mut curr_dep = PrimitiveDependency::SpatialNode { index: SpatialNodeIndex::INVALID };

        debug_assert_eq!(prev_desc.dep_count, curr_desc.dep_count);

        for _ in 0 .. prev_desc.dep_count {
            prev_dep_data = peek_from_slice(prev_dep_data, &mut prev_dep);
            curr_dep_data = peek_from_slice(curr_dep_data, &mut curr_dep);

            match (&prev_dep, &curr_dep) {
                (PrimitiveDependency::Clip { clip: prev }, PrimitiveDependency::Clip { clip: curr }) => {
                    if prev != curr {
                        return PrimitiveCompareResult::Clip;
                    }
                }
                (PrimitiveDependency::SpatialNode { index: prev }, PrimitiveDependency::SpatialNode { index: curr }) => {
                    let prev_key = SpatialNodeKey {
                        spatial_node_index: *prev,
                        frame_id: self.prev_frame_id,
                    };
                    let curr_key = SpatialNodeKey {
                        spatial_node_index: *curr,
                        frame_id: self.curr_frame_id,
                    };
                    if !spatial_node_comparer.are_transforms_equivalent(&prev_key, &curr_key) {
                        return PrimitiveCompareResult::Transform;
                    }
                }
                (PrimitiveDependency::OpacityBinding { binding: prev }, PrimitiveDependency::OpacityBinding { binding: curr }) => {
                    if prev != curr {
                        return PrimitiveCompareResult::OpacityBinding;
                    }

                    if let OpacityBinding::Binding(id) = curr {
                        if opacity_bindings
                            .get(id)
                            .map_or(true, |info| info.changed) {
                            return PrimitiveCompareResult::OpacityBinding;
                        }
                    }
                }
                (PrimitiveDependency::ColorBinding { binding: prev }, PrimitiveDependency::ColorBinding { binding: curr }) => {
                    if prev != curr {
                        return PrimitiveCompareResult::ColorBinding;
                    }

                    if let ColorBinding::Binding(id) = curr {
                        if color_bindings
                            .get(id)
                            .map_or(true, |info| info.changed) {
                            return PrimitiveCompareResult::ColorBinding;
                        }
                    }
                }
                (PrimitiveDependency::Image { image: prev }, PrimitiveDependency::Image { image: curr }) => {
                    if prev != curr {
                        return PrimitiveCompareResult::Image;
                    }

                    if resource_cache.get_image_generation(curr.key) != curr.generation {
                        return PrimitiveCompareResult::Image;
                    }
                }
                _ => {
                    // There was a mismatch between types of dependencies, so something changed
                    return PrimitiveCompareResult::Descriptor;
                }
            }
        }

        PrimitiveCompareResult::Equal
    }
}

/// Details for a node in a quadtree that tracks dirty rects for a tile.
#[cfg_attr(any(feature="capture",feature="replay"), derive(Clone))]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub enum TileNodeKind {
    Leaf {
        /// The index buffer of primitives that affected this tile previous frame
        #[cfg_attr(any(feature = "capture", feature = "replay"), serde(skip))]
        prev_indices: Vec<PrimitiveDependencyIndex>,
        /// The index buffer of primitives that affect this tile on this frame
        #[cfg_attr(any(feature = "capture", feature = "replay"), serde(skip))]
        curr_indices: Vec<PrimitiveDependencyIndex>,
        /// A bitset of which of the last 64 frames have been dirty for this leaf.
        #[cfg_attr(any(feature = "capture", feature = "replay"), serde(skip))]
        dirty_tracker: u64,
        /// The number of frames since this node split or merged.
        #[cfg_attr(any(feature = "capture", feature = "replay"), serde(skip))]
        frames_since_modified: usize,
    },
    Node {
        /// The four children of this node
        children: Vec<TileNode>,
    },
}

/// The kind of modification that a tile wants to do
#[derive(Copy, Clone, PartialEq, Debug)]
enum TileModification {
    Split,
    Merge,
}

/// A node in the dirty rect tracking quadtree.
#[cfg_attr(any(feature="capture",feature="replay"), derive(Clone))]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct TileNode {
    /// Leaf or internal node
    pub kind: TileNodeKind,
    /// Rect of this node in the same space as the tile cache picture
    pub rect: PictureBox2D,
}

impl TileNode {
    /// Construct a new leaf node, with the given primitive dependency index buffer
    fn new_leaf(curr_indices: Vec<PrimitiveDependencyIndex>) -> Self {
        TileNode {
            kind: TileNodeKind::Leaf {
                prev_indices: Vec::new(),
                curr_indices,
                dirty_tracker: 0,
                frames_since_modified: 0,
            },
            rect: PictureBox2D::zero(),
        }
    }

    /// Draw debug information about this tile node
    fn draw_debug_rects(
        &self,
        pic_to_world_mapper: &SpaceMapper<PicturePixel, WorldPixel>,
        is_opaque: bool,
        local_valid_rect: PictureRect,
        scratch: &mut PrimitiveScratchBuffer,
        global_device_pixel_scale: DevicePixelScale,
    ) {
        match self.kind {
            TileNodeKind::Leaf { dirty_tracker, .. } => {
                let color = if (dirty_tracker & 1) != 0 {
                    debug_colors::RED
                } else if is_opaque {
                    debug_colors::GREEN
                } else {
                    debug_colors::YELLOW
                };

                if let Some(local_rect) = local_valid_rect.intersection(&self.rect) {
                    let world_rect = pic_to_world_mapper
                        .map(&local_rect)
                        .unwrap();
                    let device_rect = world_rect * global_device_pixel_scale;

                    let outer_color = color.scale_alpha(0.3);
                    let inner_color = outer_color.scale_alpha(0.5);
                    scratch.push_debug_rect(
                        device_rect.inflate(-3.0, -3.0),
                        outer_color,
                        inner_color
                    );
                }
            }
            TileNodeKind::Node { ref children, .. } => {
                for child in children.iter() {
                    child.draw_debug_rects(
                        pic_to_world_mapper,
                        is_opaque,
                        local_valid_rect,
                        scratch,
                        global_device_pixel_scale,
                    );
                }
            }
        }
    }

    /// Calculate the four child rects for a given node
    fn get_child_rects(
        rect: &PictureBox2D,
        result: &mut [PictureBox2D; 4],
    ) {
        let p0 = rect.min;
        let p1 = rect.max;
        let pc = p0 + rect.size() * 0.5;

        *result = [
            PictureBox2D::new(
                p0,
                pc,
            ),
            PictureBox2D::new(
                PicturePoint::new(pc.x, p0.y),
                PicturePoint::new(p1.x, pc.y),
            ),
            PictureBox2D::new(
                PicturePoint::new(p0.x, pc.y),
                PicturePoint::new(pc.x, p1.y),
            ),
            PictureBox2D::new(
                pc,
                p1,
            ),
        ];
    }

    /// Called during pre_update, to clear the current dependencies
    fn clear(
        &mut self,
        rect: PictureBox2D,
    ) {
        self.rect = rect;

        match self.kind {
            TileNodeKind::Leaf { ref mut prev_indices, ref mut curr_indices, ref mut dirty_tracker, ref mut frames_since_modified } => {
                // Swap current dependencies to be the previous frame
                mem::swap(prev_indices, curr_indices);
                curr_indices.clear();
                // Note that another frame has passed in the dirty bit trackers
                *dirty_tracker = *dirty_tracker << 1;
                *frames_since_modified += 1;
            }
            TileNodeKind::Node { ref mut children, .. } => {
                let mut child_rects = [PictureBox2D::zero(); 4];
                TileNode::get_child_rects(&rect, &mut child_rects);
                assert_eq!(child_rects.len(), children.len());

                for (child, rect) in children.iter_mut().zip(child_rects.iter()) {
                    child.clear(*rect);
                }
            }
        }
    }

    /// Add a primitive dependency to this node
    fn add_prim(
        &mut self,
        index: PrimitiveDependencyIndex,
        prim_rect: &PictureBox2D,
    ) {
        match self.kind {
            TileNodeKind::Leaf { ref mut curr_indices, .. } => {
                curr_indices.push(index);
            }
            TileNodeKind::Node { ref mut children, .. } => {
                for child in children.iter_mut() {
                    if child.rect.intersects(prim_rect) {
                        child.add_prim(index, prim_rect);
                    }
                }
            }
        }
    }

    /// Apply a merge or split operation to this tile, if desired
    fn maybe_merge_or_split(
        &mut self,
        level: i32,
        curr_prims: &[PrimitiveDescriptor],
        max_split_levels: i32,
    ) {
        // Determine if this tile wants to split or merge
        let mut tile_mod = None;

        fn get_dirty_frames(
            dirty_tracker: u64,
            frames_since_modified: usize,
        ) -> Option<u32> {
            // Only consider splitting or merging at least 64 frames since we last changed
            if frames_since_modified > 64 {
                // Each bit in the tracker is a frame that was recently invalidated
                Some(dirty_tracker.count_ones())
            } else {
                None
            }
        }

        match self.kind {
            TileNodeKind::Leaf { dirty_tracker, frames_since_modified, .. } => {
                // Only consider splitting if the tree isn't too deep.
                if level < max_split_levels {
                    if let Some(dirty_frames) = get_dirty_frames(dirty_tracker, frames_since_modified) {
                        // If the tile has invalidated > 50% of the recent number of frames, split.
                        if dirty_frames > 32 {
                            tile_mod = Some(TileModification::Split);
                        }
                    }
                }
            }
            TileNodeKind::Node { ref children, .. } => {
                // There's two conditions that cause a node to merge its children:
                // (1) If _all_ the child nodes are constantly invalidating, then we are wasting
                //     CPU time tracking dependencies for each child, so merge them.
                // (2) If _none_ of the child nodes are recently invalid, then the page content
                //     has probably changed, and we no longer need to track fine grained dependencies here.

                let mut static_count = 0;
                let mut changing_count = 0;

                for child in children {
                    // Only consider merging nodes at the edge of the tree.
                    if let TileNodeKind::Leaf { dirty_tracker, frames_since_modified, .. } = child.kind {
                        if let Some(dirty_frames) = get_dirty_frames(dirty_tracker, frames_since_modified) {
                            if dirty_frames == 0 {
                                // Hasn't been invalidated for some time
                                static_count += 1;
                            } else if dirty_frames == 64 {
                                // Is constantly being invalidated
                                changing_count += 1;
                            }
                        }
                    }

                    // Only merge if all the child tiles are in agreement. Otherwise, we have some
                    // that are invalidating / static, and it's worthwhile tracking dependencies for
                    // them individually.
                    if static_count == 4 || changing_count == 4 {
                        tile_mod = Some(TileModification::Merge);
                    }
                }
            }
        }

        match tile_mod {
            Some(TileModification::Split) => {
                // To split a node, take the current dependency index buffer for this node, and
                // split it into child index buffers.
                let curr_indices = match self.kind {
                    TileNodeKind::Node { .. } => {
                        unreachable!("bug - only leaves can split");
                    }
                    TileNodeKind::Leaf { ref mut curr_indices, .. } => {
                        curr_indices.take()
                    }
                };

                let mut child_rects = [PictureBox2D::zero(); 4];
                TileNode::get_child_rects(&self.rect, &mut child_rects);

                let mut child_indices = [
                    Vec::new(),
                    Vec::new(),
                    Vec::new(),
                    Vec::new(),
                ];

                // Step through the index buffer, and add primitives to each of the children
                // that they intersect.
                for index in curr_indices {
                    let prim = &curr_prims[index.0 as usize];
                    for (child_rect, indices) in child_rects.iter().zip(child_indices.iter_mut()) {
                        if prim.prim_clip_box.intersects(child_rect) {
                            indices.push(index);
                        }
                    }
                }

                // Create the child nodes and switch from leaf -> node.
                let children = child_indices
                    .iter_mut()
                    .map(|i| TileNode::new_leaf(mem::replace(i, Vec::new())))
                    .collect();

                self.kind = TileNodeKind::Node {
                    children,
                };
            }
            Some(TileModification::Merge) => {
                // Construct a merged index buffer by collecting the dependency index buffers
                // from each child, and merging them into a de-duplicated index buffer.
                let merged_indices = match self.kind {
                    TileNodeKind::Node { ref mut children, .. } => {
                        let mut merged_indices = Vec::new();

                        for child in children.iter() {
                            let child_indices = match child.kind {
                                TileNodeKind::Leaf { ref curr_indices, .. } => {
                                    curr_indices
                                }
                                TileNodeKind::Node { .. } => {
                                    unreachable!("bug: child is not a leaf");
                                }
                            };
                            merged_indices.extend_from_slice(child_indices);
                        }

                        merged_indices.sort();
                        merged_indices.dedup();

                        merged_indices
                    }
                    TileNodeKind::Leaf { .. } => {
                        unreachable!("bug - trying to merge a leaf");
                    }
                };

                // Switch from a node to a leaf, with the combined index buffer
                self.kind = TileNodeKind::Leaf {
                    prev_indices: Vec::new(),
                    curr_indices: merged_indices,
                    dirty_tracker: 0,
                    frames_since_modified: 0,
                };
            }
            None => {
                // If this node didn't merge / split, then recurse into children
                // to see if they want to split / merge.
                if let TileNodeKind::Node { ref mut children, .. } = self.kind {
                    for child in children.iter_mut() {
                        child.maybe_merge_or_split(
                            level+1,
                            curr_prims,
                            max_split_levels,
                        );
                    }
                }
            }
        }
    }

    /// Update the dirty state of this node, building the overall dirty rect
    fn update_dirty_rects(
        &mut self,
        prev_prims: &[PrimitiveDescriptor],
        curr_prims: &[PrimitiveDescriptor],
        prim_comparer: &mut PrimitiveComparer,
        dirty_rect: &mut PictureBox2D,
        compare_cache: &mut FastHashMap<PrimitiveComparisonKey, PrimitiveCompareResult>,
        invalidation_reason: &mut Option<InvalidationReason>,
        frame_context: &FrameVisibilityContext,
    ) {
        match self.kind {
            TileNodeKind::Node { ref mut children, .. } => {
                for child in children.iter_mut() {
                    child.update_dirty_rects(
                        prev_prims,
                        curr_prims,
                        prim_comparer,
                        dirty_rect,
                        compare_cache,
                        invalidation_reason,
                        frame_context,
                    );
                }
            }
            TileNodeKind::Leaf { ref prev_indices, ref curr_indices, ref mut dirty_tracker, .. } => {
                // If the index buffers are of different length, they must be different
                if prev_indices.len() == curr_indices.len() {
                    // Walk each index buffer, comparing primitives
                    for (prev_index, curr_index) in prev_indices.iter().zip(curr_indices.iter()) {
                        let i0 = prev_index.0 as usize;
                        let i1 = curr_index.0 as usize;

                        // Compare the primitives, caching the result in a hash map
                        // to save comparisons in other tree nodes.
                        let key = PrimitiveComparisonKey {
                            prev_index: *prev_index,
                            curr_index: *curr_index,
                        };

                        let prim_compare_result = *compare_cache
                            .entry(key)
                            .or_insert_with(|| {
                                let prev = &prev_prims[i0];
                                let curr = &curr_prims[i1];
                                prim_comparer.compare_prim(prev, curr)
                            });

                        // If not the same, mark this node as dirty and update the dirty rect
                        if prim_compare_result != PrimitiveCompareResult::Equal {
                            if invalidation_reason.is_none() {
                                *invalidation_reason = Some(InvalidationReason::Content);
                            }
                            *dirty_rect = self.rect.union(dirty_rect);
                            *dirty_tracker = *dirty_tracker | 1;
                            break;
                        }
                    }
                } else {
                    if invalidation_reason.is_none() {
                        *invalidation_reason = Some(InvalidationReason::PrimCount);
                    }
                    *dirty_rect = self.rect.union(dirty_rect);
                    *dirty_tracker = *dirty_tracker | 1;
                }
            }
        }
    }
}

impl CompositeState {
    // A helper function to destroy all native surfaces for a given list of tiles
    pub fn destroy_native_tiles<'a, I: Iterator<Item = &'a mut Box<Tile>>>(
        &mut self,
        tiles_iter: I,
        resource_cache: &mut ResourceCache,
    ) {
        // Any old tiles that remain after the loop above are going to be dropped. For
        // simple composite mode, the texture cache handle will expire and be collected
        // by the texture cache. For native compositor mode, we need to explicitly
        // invoke a callback to the client to destroy that surface.
        if let CompositorKind::Native { .. } = self.compositor_kind {
            for tile in tiles_iter {
                // Only destroy native surfaces that have been allocated. It's
                // possible for display port tiles to be created that never
                // come on screen, and thus never get a native surface allocated.
                if let Some(TileSurface::Texture { descriptor: SurfaceTextureDescriptor::Native { ref mut id, .. }, .. }) = tile.surface {
                    if let Some(id) = id.take() {
                        resource_cache.destroy_compositor_tile(id);
                    }
                }
            }
        }
    }
}

fn get_relative_scale_offset(
    child_spatial_node_index: SpatialNodeIndex,
    parent_spatial_node_index: SpatialNodeIndex,
    spatial_tree: &SpatialTree,
) -> ScaleOffset {
    let transform = spatial_tree.get_relative_transform(
        child_spatial_node_index,
        parent_spatial_node_index,
    );
    let mut scale_offset = match transform {
        CoordinateSpaceMapping::Local => ScaleOffset::identity(),
        CoordinateSpaceMapping::ScaleOffset(scale_offset) => scale_offset,
        CoordinateSpaceMapping::Transform(m) => {
            ScaleOffset::from_transform(&m).expect("bug: pictures caches don't support complex transforms")
        }
    };

    // Compositors expect things to be aligned on device pixels. Logic at a higher level ensures that is
    // true, but floating point inaccuracy can sometimes result in small differences, so remove
    // them here.
    scale_offset.offset = scale_offset.offset.round();

    scale_offset
}

pub fn calculate_screen_uv(
    p: DevicePoint,
    clipped: DeviceRect,
) -> DeviceHomogeneousVector {
    // TODO(gw): Switch to a simple mix, no bilerp / homogeneous vec needed anymore
    DeviceHomogeneousVector::new(
        (p.x - clipped.min.x) / (clipped.max.x - clipped.min.x),
        (p.y - clipped.min.y) / (clipped.max.y - clipped.min.y),
        0.0,
        1.0,
    )
}

fn get_surface_rects(
    surface_index: SurfaceIndex,
    composite_mode: &PictureCompositeMode,
    parent_surface_index: SurfaceIndex,
    surfaces: &mut [SurfaceInfo],
    spatial_tree: &SpatialTree,
    max_surface_size: f32,
) -> Option<SurfaceAllocInfo> {
    let parent_surface = &surfaces[parent_surface_index.0];

    let local_to_parent = SpaceMapper::new_with_target(
        parent_surface.surface_spatial_node_index,
        surfaces[surface_index.0].surface_spatial_node_index,
        parent_surface.clipping_rect,
        spatial_tree,
    );

    let local_clip_rect = local_to_parent
        .unmap(&parent_surface.clipping_rect)
        .unwrap_or(PictureRect::max_rect())
        .cast_unit();

    let surface = &mut surfaces[surface_index.0];

    let (clipped_local, unclipped_local) = match composite_mode {
        PictureCompositeMode::Filter(Filter::DropShadows(ref shadows)) => {
            let local_prim_rect = surface.clipped_local_rect;

            let mut required_local_rect = match local_prim_rect.intersection(&local_clip_rect) {
                Some(rect) => rect,
                None => return None,
            };

            for shadow in shadows {
                let (blur_radius_x, blur_radius_y) = surface.clamp_blur_radius(
                    shadow.blur_radius,
                    shadow.blur_radius,
                );
                let blur_inflation_x = blur_radius_x * BLUR_SAMPLE_SCALE;
                let blur_inflation_y = blur_radius_y * BLUR_SAMPLE_SCALE;

                let local_shadow_rect = local_prim_rect
                    .translate(shadow.offset.cast_unit());

                if let Some(clipped_shadow_rect) = local_clip_rect.intersection(&local_shadow_rect) {
                    let required_shadow_rect = clipped_shadow_rect.inflate(blur_inflation_x, blur_inflation_y);

                    let local_clipped_shadow_rect = required_shadow_rect.translate(-shadow.offset.cast_unit());

                    required_local_rect = required_local_rect.union(&local_clipped_shadow_rect);
                }
            }

            let unclipped = composite_mode.get_rect(surface, None);
            let clipped = required_local_rect;

            let clipped = match clipped.intersection(&unclipped.cast_unit()) {
                Some(rect) => rect,
                None => return None,
            };

            (clipped, unclipped)
        }
        _ => {
            let surface_origin = surface.clipped_local_rect.min.to_vector().cast_unit();

            let normalized_prim_rect = composite_mode
                .get_rect(surface, None)
                .translate(-surface_origin);

            let normalized_clip_rect = local_clip_rect
                .cast_unit()
                .translate(-surface_origin);

            let norm_clipped_rect = match normalized_prim_rect.intersection(&normalized_clip_rect) {
                Some(rect) => rect,
                None => return None,
            };

            let norm_clipped_rect = composite_mode.get_rect(surface, Some(norm_clipped_rect));

            let norm_clipped_rect = match norm_clipped_rect.intersection(&normalized_prim_rect) {
                Some(rect) => rect,
                None => return None,
            };

            let unclipped = normalized_prim_rect.translate(surface_origin);
            let clipped = norm_clipped_rect.translate(surface_origin);

            (clipped.cast_unit(), unclipped.cast_unit())
        }
    };

    let (mut clipped, mut unclipped) = if surface.raster_spatial_node_index != surface.surface_spatial_node_index {
        assert_eq!(surface.device_pixel_scale.0, 1.0);

        let local_to_world = SpaceMapper::new_with_target(
            spatial_tree.root_reference_frame_index(),
            surface.surface_spatial_node_index,
            WorldRect::max_rect(),
            spatial_tree,
        );

        let clipped = (local_to_world.map(&clipped_local.cast_unit()).unwrap() * surface.device_pixel_scale).round_out();
        let unclipped = local_to_world.map(&unclipped_local).unwrap() * surface.device_pixel_scale;

        (clipped, unclipped)
    } else {
        let clipped = (clipped_local.cast_unit() * surface.device_pixel_scale).round_out();
        let unclipped = unclipped_local.cast_unit() * surface.device_pixel_scale;

        (clipped, unclipped)
    };

    let task_size_f = clipped.size();

    if task_size_f.width > max_surface_size || task_size_f.height > max_surface_size {
        let max_dimension = clipped_local.width().max(clipped_local.height()).ceil();

        surface.raster_spatial_node_index = surface.surface_spatial_node_index;
        surface.device_pixel_scale = Scale::new(max_surface_size / max_dimension);

        clipped = (clipped_local.cast_unit() * surface.device_pixel_scale).round();
        unclipped = unclipped_local.cast_unit() * surface.device_pixel_scale;
    }

    let task_size = clipped.size().to_i32();
    debug_assert!(task_size.width <= max_surface_size as i32);
    debug_assert!(task_size.height <= max_surface_size as i32);

    let uv_rect_kind = calculate_uv_rect_kind(
        clipped,
        unclipped,
    );

    // If the task size is zero sized, skip creation and drawing of it
    if task_size.width == 0 || task_size.height == 0 {
        return None;
    }

    // If the final clipped surface rect is not the same or larger as the unclipped
    // local rect of the surface, we need to enable scissor rect (which disables
    // merging batches between this and other render tasks allocated to the same
    // render target). This is conservative - we could do better in future by
    // distinguishing between clips that affect the surface itself vs. clips on
    // child primitives that don't affect this.
    let needs_scissor_rect = !clipped_local.contains_box(&surface.unclipped_local_rect);

    Some(SurfaceAllocInfo {
        task_size,
        needs_scissor_rect,
        clipped,
        unclipped,
        clipped_local,
        uv_rect_kind,
    })
}

fn calculate_uv_rect_kind(
    clipped: DeviceRect,
    unclipped: DeviceRect,
) -> UvRectKind {
    let top_left = calculate_screen_uv(
        unclipped.top_left().cast_unit(),
        clipped,
    );

    let top_right = calculate_screen_uv(
        unclipped.top_right().cast_unit(),
        clipped,
    );

    let bottom_left = calculate_screen_uv(
        unclipped.bottom_left().cast_unit(),
        clipped,
    );

    let bottom_right = calculate_screen_uv(
        unclipped.bottom_right().cast_unit(),
        clipped,
    );

    UvRectKind::Quad {
        top_left,
        top_right,
        bottom_left,
        bottom_right,
    }
}

#[test]
fn test_large_surface_scale_1() {
    use crate::spatial_tree::{SceneSpatialTree, SpatialTree};

    let mut cst = SceneSpatialTree::new();
    let root_reference_frame_index = cst.root_reference_frame_index();

    let mut spatial_tree = SpatialTree::new();
    spatial_tree.apply_updates(cst.end_frame_and_get_pending_updates());
    spatial_tree.update_tree(&SceneProperties::new());

    let map_local_to_surface = SpaceMapper::new_with_target(
        root_reference_frame_index,
        root_reference_frame_index,
        PictureRect::max_rect(),
        &spatial_tree,
    );

    let mut surfaces = vec![
        SurfaceInfo {
            unclipped_local_rect: PictureRect::max_rect(),
            clipped_local_rect: PictureRect::max_rect(),
            is_opaque: true,
            clipping_rect: PictureRect::max_rect(),
            map_local_to_surface: map_local_to_surface.clone(),
            raster_spatial_node_index: root_reference_frame_index,
            surface_spatial_node_index: root_reference_frame_index,
            device_pixel_scale: DevicePixelScale::new(1.0),
            world_scale_factors: (1.0, 1.0),
            local_scale: (1.0, 1.0),
            allow_snapping: true,
        },
        SurfaceInfo {
            unclipped_local_rect: PictureRect::new(
                PicturePoint::new(52.76350021362305, 0.0),
                PicturePoint::new(159.6738739013672, 35.0),
            ),
            clipped_local_rect: PictureRect::max_rect(),
            is_opaque: true,
            clipping_rect: PictureRect::max_rect(),
            map_local_to_surface,
            raster_spatial_node_index: root_reference_frame_index,
            surface_spatial_node_index: root_reference_frame_index,
            device_pixel_scale: DevicePixelScale::new(43.82798767089844),
            world_scale_factors: (1.0, 1.0),
            local_scale: (1.0, 1.0),
            allow_snapping: true,
        },
    ];

    get_surface_rects(
        SurfaceIndex(1),
        &PictureCompositeMode::Blit(BlitReason::ISOLATE),
        SurfaceIndex(0),
        &mut surfaces,
        &spatial_tree,
        MAX_SURFACE_SIZE as f32,
    );
}
