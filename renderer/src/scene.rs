//! Loading the scene bundle, and working out what the app can address in it.
//!
//! The scene is an editor export: `bundle/scene.toml` plus `bundle/assets/…`.
//! The renderer loads it and then tells the app two things about it, by name
//! hash (strings never cross the bridge):
//!
//! * its COLLIDERS, so the app can build its world from the authored scene;
//! * which nodes are DRIVABLE, i.e. can be the target of a `BIND` or the
//!   source of a `SPAWN`.
//!
//! What a collider or a node MEANS (floor, wall, player…) is not decided here.
//! The renderer reports names; the app gives them meaning.

use std::collections::HashMap;

use awsm_renderer::materials::MaterialKey;
use awsm_renderer::meshes::MeshKey;
use awsm_renderer::transforms::{Transform, TransformKey};
use awsm_renderer::AwsmRenderer;
use awsm_renderer_scene::{ColliderShape, EditorNode, NodeId, NodeKind, Trs};
use awsm_renderer_scene_loader::assets::HttpAssets;
use awsm_renderer_scene_loader::{load_scene_for_player, LoadedScene, ParticleDriver};
use awsm_renderer_settings::scene_anti_aliasing;
use glam::{Mat4, Quat, Vec3};

use crate::bridge::{bridge_hash, Collider, Shape};
use crate::page;

/// Everything about the loaded scene that the bridge talks about. The default
/// is an empty scene: what the renderer holds between wiping the old scene
/// and loading it again.
#[derive(Default)]
pub struct SceneIndex {
    /// Every collider node of the scene, in world space.
    pub colliders: Vec<Collider>,
    nodes: Vec<DrivableNode>,
    /// Name hash → index into `nodes`. Several names can lead to the same
    /// entry: a group and the mesh inside it both resolve to that mesh.
    by_name: HashMap<u32, usize>,
    /// The scene's particle emitters. The loader builds them; they only move
    /// when ticked, once per frame.
    pub particles: ParticleDriver,
}

/// A scene node with renderable meshes, plus everything needed to put it back
/// the way it was authored after the app has driven it.
pub struct DrivableNode {
    /// The transform the node's meshes hang from.
    pub transform: TransformKey,
    /// That transform's authored parent.
    pub parent: Option<TransformKey>,
    /// Its authored transform relative to that parent.
    pub local: Transform,
    /// Its authored transform in world space.
    pub world: Transform,
    pub meshes: Vec<NodeMesh>,
    /// The node's material variants: (hash of the variant's name, material).
    variants: Vec<(u32, MaterialKey)>,
}

pub struct NodeMesh {
    pub key: MeshKey,
    /// The material the mesh was loaded with.
    pub material: MaterialKey,
    /// Where the mesh sits relative to the node, for meshes that hang from a
    /// transform of their own below it (the parts of an imported model).
    /// `None` when the mesh hangs directly from the node's transform.
    pub offset: Option<Transform>,
}

impl SceneIndex {
    /// The drivable node a name hash refers to: the first node with that name
    /// in document order whose subtree contains a mesh.
    pub fn find(&self, name_hash: u32) -> Option<usize> {
        self.by_name.get(&name_hash).copied()
    }

    pub fn node(&self, index: usize) -> &DrivableNode {
        &self.nodes[index]
    }
}

impl DrivableNode {
    /// The material of the variant with this name hash, if the node has one.
    pub fn variant(&self, name_hash: u32) -> Option<MaterialKey> {
        self.variants
            .iter()
            .find(|(hash, _)| *hash == name_hash)
            .map(|(_, material)| *material)
    }
}

/// Fetches the bundle under `base_url` (the absolute URL of the site root,
/// ending in `/`), loads it into the renderer, and indexes it.
pub async fn load(renderer: &mut AwsmRenderer, base_url: &str) -> Result<SceneIndex, String> {
    let bundle_url = format!("{base_url}bundle");
    let scene_url = format!("{bundle_url}/scene.toml");

    // The renderer built no pipelines yet, it only reserved them. Compiling
    // them is GPU-driver work and fetching the scene is network work, so the
    // two run side by side instead of one after the other.
    page::post_progress(
        "compiling core render pipelines + fetching scene… \
         (first visit can take a while — cached after)",
    );
    let (compiled, scene) =
        futures::join!(renderer.ensure_config_pipelines(), fetch_scene(&scene_url));
    let compiled =
        compiled.map_err(|err| format!("could not compile the render pipelines: {err}"))?;
    let mut scene = scene?;
    page::post_progress(&format!(
        "core pipelines ready ({compiled}) · scene parsed ({} nodes)",
        scene.nodes.len()
    ));

    // The anti-aliasing is the page's choice (its Settings), and the renderer
    // already runs it. The loader applies the scene's authored anti-aliasing,
    // so the scene is given the renderer's: the loader then changes nothing,
    // and nothing is compiled for a mode this session does not use.
    scene.post_process.anti_aliasing = scene_anti_aliasing(&renderer.anti_aliasing);

    // The loader asks for bundle-relative paths (`assets/<id>.png`, …) and
    // this fetches them from next to `scene.toml`.
    let assets = HttpAssets::new(bundle_url);
    // Several load phases report the same counts back to back; only pass on
    // lines that changed, and skip empty phases.
    let mut last_line = String::new();
    let loaded = load_scene_for_player(renderer, &scene, &assets, |phase| {
        let line = phase.label();
        if line != last_line && !line.contains("0/0") {
            page::post_progress(&line);
            last_line = line;
        }
    })
    .await
    .map_err(|err| format!("could not load the scene: {err:#}"))?;

    // The loader only stores each node's transform relative to its parent.
    // This resolves the hierarchy, so that world transforms can be read below.
    renderer.update_transforms();

    let mut colliders = Vec::new();
    for node in &scene.nodes {
        collect_colliders(
            node,
            Mat4::IDENTITY,
            bridge_hash(&node.name),
            &mut colliders,
        );
    }

    let mut index = SceneIndex {
        colliders,
        nodes: Vec::new(),
        by_name: HashMap::new(),
        particles: ParticleDriver::new(&loaded),
    };
    index_nodes(
        &scene.nodes,
        renderer,
        &loaded,
        &mut index,
        &mut HashMap::new(),
    );
    tracing::info!(
        "scene loaded: {} colliders, {} drivable nodes, {} particle emitters",
        index.colliders.len(),
        index.nodes.len(),
        index.particles.len()
    );
    Ok(index)
}

async fn fetch_scene(url: &str) -> Result<awsm_renderer_scene::Scene, String> {
    // `NoCache` still uses the HTTP cache but revalidates first, so a
    // re-exported scene is picked up by a plain page reload.
    let response = gloo_net::http::Request::get(url)
        .cache(web_sys::RequestCache::NoCache)
        .send()
        .await
        .map_err(|err| format!("could not fetch {url}: {err}"))?;
    if !response.ok() {
        return Err(format!("could not fetch {url}: HTTP {}", response.status()));
    }
    let text = response
        .text()
        .await
        .map_err(|err| format!("could not read {url}: {err}"))?;
    awsm_renderer_scene::scene_from_toml(&text)
        .map_err(|err| format!("could not parse {url}: {err}"))
}

/// Walks `node`'s subtree and records every collider in world space.
///
/// World transforms are composed here from the authored data (parent × local
/// down the tree) rather than read back from the renderer, so this depends on
/// nothing but the scene file.
fn collect_colliders(node: &EditorNode, parent_world: Mat4, root: u32, out: &mut Vec<Collider>) {
    // A prefab subtree is a template for instances, not part of the world.
    if node.prefab {
        return;
    }
    let world = parent_world * trs_matrix(&node.transform);

    if let NodeKind::Collider(collider) = &node.kind {
        let (scale, rotation, translation) = world.to_scale_rotation_translation();
        match scaled_shape(collider, scale) {
            Some(shape) => out.push(Collider {
                name: bridge_hash(&node.name),
                root,
                shape,
                translation: translation.to_array(),
                rotation: rotation.to_array(),
            }),
            None => tracing::warn!(
                "collider '{}' has a shape the bridge has no record for; it is not reported",
                node.name
            ),
        }
    }

    for child in &node.children {
        collect_colliders(child, world, root, out);
    }
}

/// A scene collider shape as the bridge describes shapes, or `None` for the
/// kinds the bridge does not carry.
///
/// A collider has no scale of its own on the other side of the bridge, so the
/// node's accumulated world scale is folded into the dimensions. That is exact
/// for uniform scale and for axis-aligned boxes; for a round shape under
/// non-uniform scale the largest affected axis is used, which errs on the side
/// of a collider that is too big rather than too small.
fn scaled_shape(collider: &ColliderShape, scale: Vec3) -> Option<Shape> {
    let scale = scale.abs();
    // Capsules, cylinders and cones stand along local Y.
    let radial = scale.x.max(scale.z);
    Some(match *collider {
        ColliderShape::Box { half_extents } => Shape::Box {
            half_extents: (Vec3::from_array(half_extents) * scale).to_array(),
        },
        ColliderShape::Sphere { radius } => Shape::Sphere {
            radius: radius * scale.max_element(),
        },
        ColliderShape::Capsule {
            half_height,
            radius,
        } => Shape::Capsule {
            half_height: half_height * scale.y,
            radius: radius * radial,
        },
        ColliderShape::Cylinder {
            half_height,
            radius,
        } => Shape::Cylinder {
            half_height: half_height * scale.y,
            radius: radius * radial,
        },
        ColliderShape::Cone {
            half_height,
            radius,
        } => Shape::Cone {
            half_height: half_height * scale.y,
            radius: radius * radial,
        },
        _ => return None,
    })
}

/// Registers every name in `nodes` (and below) that leads to a drivable node.
///
/// `captured` remembers, per mesh node, where it landed in `index.nodes` (or
/// that it turned out not to be drivable), so a mesh reached through several
/// names is only captured once.
fn index_nodes(
    nodes: &[EditorNode],
    renderer: &AwsmRenderer,
    loaded: &LoadedScene,
    index: &mut SceneIndex,
    captured: &mut HashMap<NodeId, Option<usize>>,
) {
    for node in nodes {
        // The loader keeps prefab subtrees out of the live world.
        if node.prefab {
            continue;
        }
        let name = bridge_hash(&node.name);
        // 0 means "none" on the bridge, and the first node with a name wins.
        if name != 0 && !index.by_name.contains_key(&name) {
            if let Some(mesh_node) = first_mesh_node(node) {
                let slot = *captured.entry(mesh_node.id).or_insert_with(|| {
                    let drivable = capture(mesh_node, renderer, loaded)?;
                    index.nodes.push(drivable);
                    Some(index.nodes.len() - 1)
                });
                if let Some(slot) = slot {
                    index.by_name.insert(name, slot);
                }
            }
        }
        index_nodes(&node.children, renderer, loaded, index, captured);
    }
}

/// The first node in `node`'s subtree (itself included, depth first) that is a
/// plain mesh.
///
/// Naming a group therefore addresses the mesh inside it. Skinned and
/// cluster-LOD meshes are left out on purpose: posing one takes more than a
/// transform (a skeleton, per-mesh paging state), which the bind / spawn
/// commands do not carry.
fn first_mesh_node(node: &EditorNode) -> Option<&EditorNode> {
    if node.prefab {
        return None;
    }
    if matches!(node.kind, NodeKind::Mesh { .. }) {
        return Some(node);
    }
    node.children.iter().find_map(first_mesh_node)
}

/// Records a mesh node's renderer handles and its authored state. Must run
/// while the scene is still exactly as loaded.
fn capture(
    node: &EditorNode,
    renderer: &AwsmRenderer,
    loaded: &LoadedScene,
) -> Option<DrivableNode> {
    let handles = loaded.nodes.get(&node.id)?;
    let transforms = &renderer.transforms;
    let world = *transforms.get_world(handles.transform).ok()?;

    let meshes: Vec<NodeMesh> = handles
        .meshes
        .iter()
        .filter_map(|&key| {
            let mesh = renderer.meshes.get(key).ok()?;
            let offset = if mesh.transform_key == handles.transform {
                None
            } else {
                let mesh_world = *transforms.get_world(mesh.transform_key).ok()?;
                Some(Transform::from(world.inverse() * mesh_world))
            };
            Some(NodeMesh {
                key,
                material: mesh.material_key,
                offset,
            })
        })
        .collect();
    if meshes.is_empty() {
        return None;
    }

    let variants: Vec<(u32, MaterialKey)> = loaded
        .node_material_variants
        .get(&node.id)
        .map(|variants| {
            variants
                .iter()
                .map(|variant| (bridge_hash(&variant.name), variant.key))
                .collect()
        })
        .unwrap_or_default();

    Some(DrivableNode {
        transform: handles.transform,
        parent: transforms.get_parent(handles.transform).ok(),
        local: transforms.get_local(handles.transform).ok()?.clone(),
        world: Transform::from(world),
        meshes,
        variants,
    })
}

fn trs_matrix(trs: &Trs) -> Mat4 {
    Mat4::from_scale_rotation_translation(
        Vec3::from_array(trs.scale),
        Quat::from_array(trs.rotation),
        Vec3::from_array(trs.translation),
    )
}
