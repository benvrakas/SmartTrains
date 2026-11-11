# Dumps the vanilla railroad signal assets (components, meshes, materials, descriptors, recipes, schematics) to
# dump_signal_assets.txt next to this script. Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>.
import os
import unreal

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dump_signal_assets.txt")
lines = []


def out(s=""):
    lines.append(str(s))


def fmt(v, depth=0):
    if v is None:
        return "None"
    if isinstance(v, (list, tuple, unreal.Array)):
        return "[" + ", ".join(fmt(x, depth + 1) for x in list(v)[:40]) + "]"
    if isinstance(v, unreal.Text):
        return '"' + str(v) + '"'
    if isinstance(v, unreal.Object):
        try:
            return v.get_path_name()
        except Exception:
            return str(v)
    if isinstance(v, unreal.StructBase) and depth < 2:
        parts = []
        for n in dir(v):
            if n.startswith("_") or callable(getattr(type(v), n, None)):
                continue
            try:
                parts.append(n + "=" + fmt(v.get_editor_property(n), depth + 1))
            except Exception:
                pass
        return type(v).__name__ + "(" + ", ".join(parts[:30]) + ")"
    return str(v)


def dump_props(obj, title):
    out("--- " + title + " : " + obj.get_class().get_path_name())
    names = sorted(set(n for n in dir(obj) if not n.startswith("_")))
    for n in names:
        try:
            attr = getattr(type(obj), n, None)
            if callable(attr):
                continue
            v = obj.get_editor_property(n)
        except Exception:
            continue
        out("  " + n + " = " + fmt(v))


def class_chain(cls):
    chain = []
    c = cls
    while c is not None:
        chain.append(c.get_path_name())
        try:
            c = unreal.SystemLibrary.get_super_class(c) if hasattr(unreal.SystemLibrary, "get_super_class") else None
        except Exception:
            c = None
    return chain


def supers(cls):
    names = []
    c = cls
    for _ in range(12):
        try:
            names.append(c.get_path_name())
            c = c.get_super_class() if hasattr(c, "get_super_class") else None
        except Exception:
            break
        if c is None:
            break
    return names


def dump_blueprint(path):
    out("")
    out("==================== " + path)
    bp = unreal.load_asset(path)
    if bp is None:
        out("  (not found)")
        return
    gen = bp.generated_class()
    out("parent chain: " + " <- ".join(supers(gen)))
    cdo = unreal.get_default_object(gen)
    dump_props(cdo, "CDO")
    sub = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = sub.k2_gather_subobject_data_for_blueprint(bp)
    for h in handles:
        data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h)
        obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
        if obj is None or not isinstance(obj, unreal.ActorComponent):
            continue
        out("")
        out("  component " + obj.get_name() + " (" + obj.get_class().get_name() + ")")
        for n in ["static_mesh", "relative_location", "relative_rotation", "relative_scale3d", "override_materials",
                  "draw_size", "widget_class", "pivot", "box_extent", "attach_parent", "mobility", "visible",
                  "hidden_in_game", "collision_profile_name"]:
            try:
                out("    " + n + " = " + fmt(obj.get_editor_property(n)))
            except Exception:
                pass
        try:
            out("    attach_parent(scene) = " + fmt(obj.get_attach_parent()))
        except Exception:
            pass


def dump_mesh(path):
    out("")
    out("==================== MESH " + path)
    m = unreal.load_asset(path)
    if m is None:
        out("  (not found)")
        return
    b = m.get_bounds()
    out("  bounds origin=" + fmt(b.origin) + " extent=" + fmt(b.box_extent))
    out("  lods=" + str(m.get_num_lods()) + " sections(lod0)=" + str(m.get_num_sections(0)))
    for i, sm in enumerate(m.get_editor_property("static_materials")):
        out("  slot " + str(i) + " name=" + str(sm.get_editor_property("material_slot_name")) + " material=" +
            fmt(sm.get_editor_property("material_interface")))
    for s in range(m.get_num_sections(0)):
        try:
            verts, tris, normals, uvs, tangents = unreal.ProceduralMeshLibrary.get_section_from_static_mesh(m, 0, s)
        except Exception as e:
            out("  section " + str(s) + " read failed: " + str(e))
            continue
        if not verts:
            out("  section " + str(s) + " empty")
            continue
        xs = [v.x for v in verts]; ys = [v.y for v in verts]; zs = [v.z for v in verts]
        us = [u.x for u in uvs] if uvs else [0]; vs = [u.y for u in uvs] if uvs else [0]
        nx = sum(n.x for n in normals) / len(normals); ny = sum(n.y for n in normals) / len(normals)
        nz = sum(n.z for n in normals) / len(normals)
        out("  section %d verts=%d tris=%d X[%.1f..%.1f] Y[%.1f..%.1f] Z[%.1f..%.1f] U[%.3f..%.3f] V[%.3f..%.3f] avgN=(%.2f,%.2f,%.2f)" % (
            s, len(verts), len(tris) // 3, min(xs), max(xs), min(ys), max(ys), min(zs), max(zs), min(us), max(us),
            min(vs), max(vs), nx, ny, nz))


def dump_material(path):
    out("")
    out("==================== MATERIAL " + path)
    m = unreal.load_asset(path)
    if m is None:
        out("  (not found)")
        return
    out("  class=" + m.get_class().get_name())
    lib = unreal.MaterialEditingLibrary
    for fn in ["get_texture_parameter_names", "get_scalar_parameter_names", "get_vector_parameter_names",
               "get_static_switch_parameter_names"]:
        try:
            out("  " + fn + " = " + fmt(getattr(lib, fn)(m)))
        except Exception as e:
            out("  " + fn + " failed: " + str(e))
    if isinstance(m, unreal.MaterialInstance):
        out("  parent=" + fmt(m.get_editor_property("parent")))
        for p in m.get_editor_property("texture_parameter_values"):
            out("  tex param " + fmt(p.get_editor_property("parameter_info")) + " = " + fmt(p.get_editor_property("parameter_value")))
    if isinstance(m, unreal.Material):
        for n in ["blend_mode", "shading_model", "two_sided", "material_domain"]:
            try:
                out("  " + n + " = " + fmt(m.get_editor_property(n)))
            except Exception:
                pass


def dump_cdo(path, title):
    out("")
    out("==================== " + title + " " + path)
    bp = unreal.load_asset(path)
    if bp is None:
        out("  (not found)")
        return
    gen = bp.generated_class() if hasattr(bp, "generated_class") else bp
    out("parent chain: " + " <- ".join(supers(gen)))
    dump_props(unreal.get_default_object(gen), "CDO")


def find_assets(substr, root="/Game/FactoryGame"):
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    res = []
    for a in reg.get_assets_by_path(root, recursive=True):
        n = str(a.asset_name)
        if substr.lower() in n.lower():
            res.append(str(a.package_name) + "." + n)
    return res




def dump_cdo_components(path):
    bp = unreal.load_asset(path)
    if bp is None:
        return
    cdo = unreal.get_default_object(bp.generated_class())
    out("")
    out("  CDO components of " + path)
    try:
        comps = cdo.get_components_by_class(unreal.ActorComponent)
    except Exception as e:
        out("    failed: " + str(e))
        comps = []
    for c in comps:
        out("    " + c.get_name() + " (" + c.get_class().get_name() + ")")
        for n in ["static_mesh", "relative_location", "relative_rotation", "relative_scale3d", "override_materials", "mobility", "visible"]:
            try:
                out("      " + n + " = " + fmt(c.get_editor_property(n)))
            except Exception:
                pass


try:
    base = "/Game/FactoryGame/Buildable/Factory/Train/Signal/"
    for n in ["Build_RailroadBlockSignal", "Build_RailroadPathSignal"]:
        dump_blueprint(base + n + "." + n)
        dump_cdo_components(base + n + "." + n)
    for n in ["Desc_RailroadBlockSignal", "Desc_RailroadPathSignal"]:
        dump_cdo(base + n + "." + n, "DESCRIPTOR")
    for n in ["Recipe_RailroadBlockSignal", "Recipe_RailroadPathSignal"]:
        dump_cdo(base + n + "." + n, "RECIPE")
    for n in ["MM_TrainSignal", "MI_TrainSignal_Green", "MI_TrainSignal_Red", "MI_TrainSignal_Off", "MI_TrainSignal_Error"]:
        dump_material(base + n + "." + n)
    out("")
    out("signal meshes: " + ", ".join(find_assets("Signal", "/Game/FactoryGame/Buildable/Factory/Train")))
    out("signal holograms: " + ", ".join(find_assets("Signal", "/Game/FactoryGame/Buildable/-Shared") + find_assets("Holo_Railroad", "/Game/FactoryGame")))
    out("railroad schematics: " + ", ".join(find_assets("Rail", "/Game/FactoryGame/Schematics") + find_assets("Train", "/Game/FactoryGame/Schematics")))
    out("signal icons: " + ", ".join(find_assets("Signal", "/Game/FactoryGame/Buildable/Factory/Train")))
except Exception as e:
    import traceback
    out("FAILED: " + traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write(chr(10).join(lines))
unreal.log("dump_signal_assets wrote " + OUT)
