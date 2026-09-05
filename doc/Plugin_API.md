# Prusa Slicer Plugin API

This is first version of Slice Plugin API, providing minimal operations to create new objects in Slicer project. 
This document should provide basic information on how plugin works in Prusa Slicer and how to create one.

## Getting started

To quickly create a hello world plugin, the Prusa Slicer CLI provides `plugin init` subcommand, so you can do 
something like this in your terminal:

```bash
cd <your_plugin_workspace>
/Applications/PrusaSlicer.app/Contents/MacOS/PrusaSlicer plugin init com.example.my-plugin 
```

Note: the snippet above is from macos and PrusaSlicer installed as ` /Applications/PrusaSlicer.app`, you may need
to provide path to you installation `PrusaSlicer` executable.

This command above will start interactive CLI wizard asking info about your new plugin and will create single lua file
with hello world like plugin in `com.example.my-plugin`. The info you entered can be changed in `manifest.json` file. 
Tip: for license field you can use Tab key to complete (or list known license identifiers).

To run the plugin in PrusaSlicer you will need to symlink (or copy) the `com.example.my-plugin` 
to `<config_dir>/lua/com.example.my-plugin` and run Plugins -> Rescan menu item command in Prusa Slicer. 


## Plugin anatomy

- Plugins are grouped into _plugin bundles_ (a directory, e.g. `com.prusa3d.slicer.calibratuin`)
- Plugin bundles contains `manifest.json` metadata file and one or more plugins
- Each plugin is single .lua file located under specific directory (e.g. `(datadir)/lua` or `(configdir)/lua`).
- Plugin file has to define `info` variable with description of the plugin.
- Plugin file has to define the entry point function of its type, that runs the plugin logic
  (`execute` for `project.plugin`, see [Plugin types](#plugin-types)).

### Plugin Bundle Metadata `manifest.json`

The `manifest.json` file describes the plugin bundle.

Here is an example of bundled plugin manifest:

```
{
	"id": "com.prusa3d.slicer.calibration",
	"name": "Calibration patterns",
	"license": "AGPL-3.0-only",
	"min_slicer_version": "3.0.0",
	"version": "1.0.0",
	"author": "prusa3d",
	"description": "Calibration patterns",
	"required_apis": {
		"project.plugin": "1.0.0"
	}
}
```

The recognized `required_apis` keys are `project.plugin`, `slicing.island_order`,
`slicing.island_sequence`, `slicing.extrusion_filter` and `slicing.fill_planner`,
all at version `1.0.0`.

This is list of recognized `manifest.json` fields. 

| Key                  | Required | Description                                                                         |
|:---------------------|:---------|:------------------------------------------------------------------------------------|
| `id`                 | Yes      | Unique identifer of plugin bundle in reverse DNS form  e.g. `com.example.my-plugin` |
| `name`               | Yes      | Human readable name of plugin bundle                                                |
| `license`            | Yes      | [SPDX identifier](https://spdx.org/licenses/) of license                            |
| `min_slicer_version` | Yes      | Minimal version of Prusa Slicer (e.g. `3.0.0`)                                      |
| `version`            | Yes      | Version of the plugin bundle                                                        |
| `author`             | Yes      | Unique author identifier (e.g. Prusa Account handle)                                |
| `description`        | No       | Description of the plugin bundle                                                    |
| `required_apis`      | Yes      | Map of Plugin APIs (key) and its required minimal version (value)                   |
| `category`           | No       | Category identifier                                                                 |
| `web`                | No       | Plugin bundle hompage web URL                                                       |
| `repo`               | No       | Plugin bundle source code repository URL                                            |                


### Plugin Metadata `info` structure

The table `info` describes plugin with following keys:
- `id` (string) plugin unique identifier, recommended is reverse domain name like notation
- `type` (string) type of plugin, allowed values are `'project.plugin'`,
  `'slicing.island_order'`, `'slicing.island_sequence'`, `'slicing.extrusion_filter'`,
  `'slicing.fill_planner'` and `'slicing.object_labels'`.
- `title` (string) displayed plugin name
- `menu` (string) menu item path to register the plugin under _Plugins_ menu item (e.g. `Calibration/My cool pattern`).
  Only used by `project.plugin`.
- `params` (array) list of parameter descriptions with following keys:
  - `name` (string) name of key in table as first argument passed to the `execute()` function.
  - `label` (string) displayed name in UI 
  - `type` (string) type of value / UI control, allowed values are:
    - `float` (UI: number input)
    - `int` (UI: number input)
    - `bool` (UI: checkbox)
  - `default` (number or string) default value

### Plugin `execute` function

The function `execute(params)` takes single argument,a table based upon description in the `info.params`. 
Values was filled prior calling the function, by user in UI constructed according the same description.


### Complete minimal example

This is the legendary hello world as a Slicer Plugin:

```lua
info = {
    id = "com.prusa3d.slicer.hello_world",
    type = "project.plugin",
    title = "Hello world",
    menu = "Minimal/Hello world",
    params = {
        {name = "num", label = "Your lucky number", type = "int", default = 42}
    }
}

function execute(params) 
    print("Hello no " .. params.num .. "!")
end
```

Once scanned, it should appear in the main menu under _Plugins_ → _Minimal_ → _Hello world_. 
After activation a simple UI will appear, with single input item of given label, so the user can pass an integer number.
The number can be than read by script as `params.num` in the `print` statement.

### Security model

The plugin runtime is *intentionally limited and sandboxed*. 

There are two main restrictions to be aware of:
- no standard `os` and `io` modules are available,
- plugin can access (via `emboss_svg`, `load_stl` and `require`) only files that are in the same directory 
  as the plugin .lua file itself. 

## Plugin types

### `project.plugin`

Runs on demand from the _Plugins_ menu and builds on the project: it can add objects,
insert custom G-code on a layer and so on. Its entry point is `execute(params)`, described
above.

### `slicing.island_order`

Hooks into slicing rather than into the GUI: it decides the order in which the disjoint
islands of a layer are printed. A layer of an object may fall apart into several separate
regions — think of a model that splits into towers above a common base. By default their
order is chained once, at slicing time, before it is known where the print head will be
when the layer starts, so the head tends to return to the same island at the beginning of
every layer. A plugin of this type is asked during G-code export instead, where the head
position is known, and can order the islands to shorten the travels between them.

The plugin has to define `order_islands(islands, ctx)`:

```lua
info = {
    id = "island_order",
    type = "slicing.island_order",
    title = "Shortest travel island order"
}

function order_islands(islands, ctx)
    -- islands[i] = {
    --     centroid = {x = <mm>, y = <mm>},
    --     bbox = {min_x = <mm>, min_y = <mm>, max_x = <mm>, max_y = <mm>}
    -- }
    -- ctx = {
    --     layer_id    = <integer>,
    --     print_z     = <mm>,
    --     extruder_id = <integer>,
    --     head        = {x = <mm>, y = <mm>}   -- nil before anything has been extruded
    -- }
    local order = {}
    for i = 1, #islands do
        order[i] = i
    end
    return order    -- the stock order
end
```

Coordinates are in millimetres, in the same frame as `ctx.head`, so a plugin does not need
to know about object instance offsets.

The return value is an array of **positions into `islands`**, 1-based, each appearing
exactly once — a permutation of `1..#islands`, not a set of slicer island identifiers.
Anything else is rejected and the stock order is used instead. An error raised by the
plugin is reported once and the stock order is used for the rest of the export, so a
broken plugin can never fail a slice.

`order_islands` is called once per layer and per object instance, and only for layers that
hold at least two islands. Calls belonging to one print arrive serialized and in layer
order. Two beds may be sliced at the same time, but each of them gets its own Lua state,
so a plugin never has to think about concurrency.

At most one `slicing.island_order` plugin is used at a time. If several are installed, the
first by plugin id wins and the others are ignored with a warning in the log.

### `slicing.island_sequence`

Builds a cross-layer schedule for connected islands of one object. It is intended for
parts which share a base and later split into independent branches. Its entry point is
`plan_islands(layers, ctx)`:

```lua
info = {
    id = "island_sequence",
    type = "slicing.island_sequence",
    title = "Sequential upper islands"
}

function plan_islands(layers, ctx)
    -- layers[i] = {
    --   layer_id = <integer>, print_z = <mm>, height = <mm>,
    --   islands = {{
    --     centroid = {x = <mm>, y = <mm>},
    --     bbox = {min_x = <mm>, min_y = <mm>, max_x = <mm>, max_y = <mm>},
    --     overlaps_below = {<1-based island positions on layer i-1>},
    --     overlaps_above = {<1-based island positions on layer i+1>}
    --   }, ...}
    -- }
    -- ctx = {
    --   printer_model = <string>,
    --   extruder_clearance_radius = <mm>,
    --   extruder_clearance_height = <mm>,
    --   collision_model = "coreone_fallback_v1" or "unchecked"
    -- }
    return {
        steps = {
            {layer = 1, islands = {1}},
            {layer = 2, islands = {1}},
            {layer = 2, islands = {2}}
        },
        wipe_distance = 2.0, -- mm, allowed range 0..20
        z_clearance = 0.6    -- mm, allowed range 0..10
    }
end
```

`layer` and every item in `islands` are 1-based positions into the supplied arrays. A
valid schedule emits every island exactly once. Before an island is emitted, all its
`overlaps_below` dependencies must already have been emitted. Invalid plans and `nil`
both preserve normal layer-by-layer printing; a Lua error disables the plugin for that
export.

The 1.0.0 engine intentionally applies a plan only to one object with one instance and no
supports, wipe tower, infinite skirt, spiral vase, ordinary complete-objects mode, or
height-based custom G-code. A branch change is performed by retracting with a capped
wipe on the last extrusion, raising Z, moving XY above the next branch, and only then
descending. The first island step after a downward move emits infill before perimeters,
using that hidden extrusion as a longer model-contained wipe.

For `printer_model == "COREONE"`, the engine checks the returned schedule with the same
three-slice fallback geometry as PrusaSlicer's sequential-object arranger: a 10 x 10 mm
nozzle footprint at 0 mm, the configured clearance-radius square at 1 mm, and an X gantry
at the configured clearance height. A colliding plan is rejected. Other printers still
receive `collision_model = "unchecked"`, and an accepted plan adds a high-severity warning
because the ordinary complete-objects checker does not model islands starting above the
bed.

At most one `slicing.island_sequence` plugin is used at a time. It may coexist with a
`slicing.island_order` plugin; the latter sees only the islands selected for the current
schedule step.

### `slicing.extrusion_filter`

Decides whether a single extrusion is printed at all.

A fill pattern clipped against a contour leaves stubs behind: an infill line that
survives only a millimetre in a corner, an internal perimeter reduced to a sliver. The
stub itself is nothing, but the head still has to travel to it, retract, extrude, retract
again and travel away. On a tall part with four such corners that repeats on every layer.

The plugin has to define `keep_extrusion(path)`:

```lua
info = {
    id = "short_extrusion",
    type = "slicing.extrusion_filter",
    title = "Drop short extrusions"
}

function keep_extrusion(path)
    -- path = {
    --     role        = <string>,    -- see the list below
    --     length      = <mm>,        -- along the path's own geometry
    --     layer_id    = <integer>,
    --     print_z     = <mm>,
    --     extruder_id = <integer>
    -- }
    if path.role == "ExternalPerimeter" or path.role == "TopSolidInfill" then
        return true
    end
    return path.length >= 2.0
end
```

Return `true` to print the path, `false` to drop it. Anything that is not a boolean is
reported once and every extrusion is kept for the rest of the export, as is an error
raised by the plugin, so a broken plugin can never fail a slice.

`role` is one of `Perimeter`, `ExternalPerimeter`, `OverhangPerimeter`, `InternalInfill`,
`SolidInfill`, `TopSolidInfill`, `Ironing`, `BridgeInfill`, `GapFill`, `Skirt`,
`SupportMaterial`, `SupportMaterialInterface`, `WipeTower`, `Custom`. These are stable
identifiers, not the translated names the UI shows.

The plugin is asked about every perimeter and infill path as the extrusions of a layer
are collected, *before* travels and seams are decided, so a rejected path disappears
together with the movement that would have reached it, and the paths that remain are
routed as if it had never existed. Calls belonging to one print arrive serialized and in
layer order; concurrently exported beds each get their own Lua state.

Note that this is called once per extrusion path rather than once per layer, which is a
few thousand calls on a typical print. Keep the function cheap.

### `slicing.fill_planner`

Generates the fill paths of one surface in place of the stock pattern.

The stock patterns run straight lines in a single direction chosen per surface, which is
wrong for some regions. A bridge over an annular gap is the motivating case: filled with
parallel lines the lines nearest the inner island become long chords spanning the whole
opening, while the same area covered by spokes has no unsupported run longer than the gap.

The plugin has to define `plan_fill(surface)`:

```lua
info = {
    id = "radial_bridge",
    type = "slicing.fill_planner",
    title = "Radial bridges"
}

function plan_fill(surface)
    -- surface = {
    --     role         = <string>,   -- as for slicing.extrusion_filter
    --     layer_id     = <integer>,
    --     print_z      = <mm>,
    --     extruder_id  = <integer>,
    --     spacing      = <mm>,       -- between two adjacent fill lines
    --     bridge_angle = <radians>,  -- the direction the slicer chose, -1 if not a bridge
    --     contour      = {{x = <mm>, y = <mm>}, ...},              -- outer boundary
    --     holes        = {{{x = <mm>, y = <mm>}, ...}, ...}        -- inner boundaries
    -- }
    -- return a list of paths, {{{x, y}, ...}, ...}, or nil
end
```

Return `nil` to keep the paths the slicer generated; that is also what happens when the
plugin raises or answers with something that is not a list of paths, so a broken plugin
can never fail a slice. Returned paths are clipped to the surface, and a plugin whose
paths clip away to nothing has the stock paths used instead.

A plugin that lays its paths out at some other line spacing than the slicer assumed can
say so, by answering with a table instead of a bare list:

```lua
    return {
        paths      = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}},
        flow_ratio = 0.9   -- optional, 1.0 by default
    }
```

`flow_ratio` multiplies the extrusion the slicer computed for the surface, the way
`bridge_flow_ratio` does for the stock pattern: the cross section follows the ratio, and
the width follows its square root for a bridge and the ratio itself otherwise. It exists
because the flow follows from the spacing - the slicer works out how much plastic a
millimetre of path has to carry to cover the surface at `surface.spacing`, and a fan of
spokes has no single spacing at all. Values outside 0.05 to 5.0 are clamped, and a
`flow_ratio` that is not a number is refused along with the rest of the answer.

The region is given already grown over its anchors, so for a bridge the paths may and
should reach out to its edge. Fills generated by Arachne carry a width per point, which
this contract cannot express, and are never offered to the plugin.

**This hook is called concurrently.** Unlike the other slicing hooks, which run in the
serialized G-code stage, `plan_fill()` is reached from the slicer's parallel infill stage,
once per surface across all layers at once. Calls are serialized on the way into Lua, so a
plugin does not have to be thread safe, but a plugin that answers for every surface of a
large print will hold that stage up. Claim only the surfaces you can improve.

### `slicing.object_labels`

Names the parts of a print that are not model objects, so the printer can cancel them
during a print the way it cancels an object.

Firmware that can cancel an object mid print - `M486` on Marlin and Prusa Buddy,
`EXCLUDE_OBJECT` on Klipper, `; printing object` comments for OctoPrint - works from a
list the slicer writes into the G-code, and that list holds model objects only. Whatever
else the printer lays down belongs to no object and cannot be cancelled. The wipe tower is
the case that motivated this: cancel the last object that needed a second filament and the
tower carries on being built, by itself, for however long the print had left.

The plugin has to define `label_region(region)`:

```lua
info = {
    id = "wipe_tower_cancel",
    type = "slicing.object_labels",
    title = "Cancellable wipe tower"
}

function label_region(region)
    -- region = {
    --     kind         = <string>,   -- "wipe_tower" is the only kind so far
    --     default_name = <string>,   -- what the slicer calls it
    --     outline      = {{x = <mm>, y = <mm>}, ...}   -- its footprint on the bed
    -- }
    -- return the name to list it under, or nil to leave it unlabelled
end
```

`label_region()` is called once per region per export, before any layer is generated.
Return `nil` to leave that part of the print as the stock slicer writes it; that is also
what happens when the plugin raises, answers with something that is not a string, or
answers with a name that is nothing but spaces.

The engine owns the syntax. A named region is defined in the same header, in the same
dialect and with an identifier that runs on from the objects', and its label is opened and
closed around its extrusions - so the objects keep the numbers they would have had, and a
plugin does not have to know one firmware's dialect from another's. What the plugin
chooses is which parts get a label and what it says. The name reaches the printer, and
Klipper's restrictions on it are applied by the engine.

Around the wipe tower the boundaries are drawn deliberately: the travel to the tower, the
acceleration around it and, above all, the tool change spliced into the middle of it stay
**outside** the label. A firmware skipping a cancelled tower therefore never skips a `T`
command, and never leaves the printer at the tower's acceleration for the object that
follows.

What a cancelled tower does skip is the ramming and the purge, and that is not a detail:
the purge is where the previous filament leaves the nozzle, so a tower cancelled with tool
changes still to come pushes the old colour out **on the object instead**, over a long
stretch of extrusion. Naming the wipe tower therefore raises a `WipeTowerCancellable`
slicing warning on every export, because the choice is made on the printer hours later,
where there is nothing left to warn anyone.

This hook does nothing unless object labelling is switched on: Print Settings >
**Precision & Slicing** > *Resolution & G-code Data* > Label objects
(`gcode_label_objects`), which is where the object list itself comes from. It is where the
option lives in 3.x; in 2.x it was under Output options.

## Plugin API

Plugin API reference is located [here](https://prusa.io/ps-plugins/)

## Plugin distribution

At the moment plugins can be distributed as signed zip files. The plugin author needs to generate her public and private 
RSA keys to create the plugin distribution zip. There is `PrusaSlicer plugin keygen` utility (or you can use `openssl` 
CLI tools). Generating keys is one-time action, you don't need to do again for another plugin bundle. You will need to 
distribute your *public* key named as `<author>.pem`, where `<author>` is value of `author` field in `manifest.json` file.
The *public key* file distribution is again a one-time action.

Following command generates for you private key (the one to **keep secret**) in file `the.author.private.pem`, 
and public key (the one to *distribute*) in file `the.author.public.pem`:

```
<path to you installation>/PrusaSlicer plugin keygen -P the.author.private.pem -p the.author.public.pem
```

Finally, to create sign zip file to distribute the plugin to users, you can run following command (assuming the files 
are named same as in the example commands above):

```
<path to you installation>/PrusaSlicer plugin sign -P the.author.private.pem com.example.my-plugin
```

The output of this command is `com.example.my-plugin.zip` file to distribute.
