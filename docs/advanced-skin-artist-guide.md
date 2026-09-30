# Advanced Skin Artist Guide

Start with a working skin NIF. Enable Advanced Skin on one material, adjust roughness while looking at it in-game, then add textures only where you need painted control. A body, face, hands and feet may use separate files and materials.

You can work entirely in the game's **CS Editor → Skin Editor**, or edit the NIF in **NifSkope**. Both routes produce ordinary NIF and DDS files. ESP patches are only needed when changing which assets a race, armor or actor uses. You do not need Python or material JSON.

These instructions describe the implementation in this branch. The complete NifSkope/editor/game workflow has not yet been validated in-game. Use a test installation containing this implementation; older Advanced Skin releases use different data.

## Route 1: use the in-game editor

1. Open **Skin Editor** and choose **Open NIF**. Select the skin material in **NIF material**. For an installed character, **Player** or **Console selection** finds loaded surfaces; **Edit source NIF** opens the verified source. **First person** discovers first-person surfaces when they are loaded.
2. Check **Enable Advanced Skin on this material**. Begin with Roughness around 0.6, Reflectance around 0.028 and Fuzz around 0.25. Leave the extra textures empty initially.
3. Choose a **Preview surface** and enable **Preview draft on selected surface**. The preview is temporary. Move sliders, use **Undo** / **Redo**, and compare live or forced wetness. Closing the editor turns the preview off.
4. Use **Choose DDS** for Skin controls, Wet surface or Detail normal + mask. If the texture is outside the game's texture directory, the editor asks where to import it under `Data/textures`. A mod manager may redirect that directory. An advanced resource-path field is also available for textures already installed in an archive.
5. Use **Save NIF as...** to save the asset. To distribute its new textures too, use **Save NIF with Skin textures...**, choose a file beneath your mod's `meshes` folder, review the displayed output list and choose **Write listed files**.

If several shapes use the same material, its changes affect them together. Expand **Shapes using this material** and choose **Make independent** for a shape that needs different settings. This prepares a separate material; save the NIF to commit it.

A saved NIF does not rebuild a character that is already loaded. Preview lets you inspect the draft immediately; rebuild/reload the model normally to check the installed asset. Do not assume a working preview proves the correct source file won your mod conflicts.

Use **New material template** to create a reusable preset without geometry. Under **Material presets**, save or import a material-only NIF and select which settings or textures to copy. This preset does not replace a body mesh.

**Batch asset output** applies the current draft to an explicit list of NIFs. Choose the material and output file for every body, hand, foot, face or first-person variant. Review the list before publishing. BodySlide or FaceGen regeneration may overwrite these outputs; apply the material again to regenerated files or maintain it in the relevant source workflow.

## Route 2: edit the NIF in NifSkope

### Select the existing skin material

Work on a copy of the final Skyrim SE NIF. Select the skin shape and follow its `BSLightingShaderProperty` link. Its native shader should already be **Face Tint (4)** or **Skin Tint (5)**. Keep its native flags, UV settings and ordinary skin textures. This does not convert hair or clothing into skin.

The settings below belong in the **shader property's Extra Data List**. They do not belong on the root, the shape or a bone.

### Add the enable field and parameters

Insert a `NiIntegerExtraData` block, set its **Name** to `CS_SkinVersion` and **Integer Data** to `1`, then add a reference to it in the shader property's Extra Data List. This number identifies the stored material format.

For each parameter you want to change, insert the listed extra-data block, enter its Name and Float Data or Integer Data, and reference it from that same list. Resize the property's list when adding references. Use one block per field; edit an existing field instead of duplicating it. NifSkope menu wording varies, but these block types and field names are the file data.

You can also open the supplied `meshes/CS/Skin/SkinMaterial.nif` alongside your model and copy its typed extra-data blocks. Link the copied blocks to your existing shader property. Keep the target property's native settings.

| What you adjust             | Name                     | Type               | Starting value | Allowed values |
| --------------------------- | ------------------------ | ------------------ | -------------: | -------------- |
| Broadness of highlights     | CS_SkinRoughness         | NiFloatExtraData   |            0.6 | 0–1            |
| Normal-incidence reflection | CS_SkinReflectance       | NiFloatExtraData   |          0.028 | 0–0.08         |
| Fine fuzz sheen             | CS_SkinFuzz              | NiFloatExtraData   |           0.25 | 0–1            |
| Allow detail normals        | CS_SkinDetailEnabled     | NiIntegerExtraData |              1 | 0 or 1         |
| Custom detail strength      | CS_SkinDetailStrength    | NiFloatExtraData   |           0.25 | 0–1            |
| Custom detail repeats       | CS_SkinDetailTiling      | NiFloatExtraData   |             10 | 0.1–100        |
| Post-process SSS strength   | CS_SkinSSSAmount         | NiFloatExtraData   |              1 | 0–1            |
| Light transmission          | CS_SkinTransmission      | NiFloatExtraData   |            0.1 | 0–1            |
| Transmission depth          | CS_SkinTransmissionDepth | NiFloatExtraData   |            0.2 | 0.001–1        |
| Response to wetness         | CS_SkinWetResponse       | NiFloatExtraData   |              1 | 0–1            |

Fields you omit use these starting values. More roughness broadens highlights; reflectance changes their intensity. Transmission depth controls how far the backlighting effect penetrates. SSS and transmission are separate controls.

### Assign ordinary texture slots

Follow the property's **Texture Set** link to `BSShaderTextureSet`. Expand its nine-entry Textures array. Numbers here start at zero; slot 5 is the sixth entry.

| NIF slot | Optional texture                 | Paint these channels                                                                     |
| -------- | -------------------------------- | ---------------------------------------------------------------------------------------- |
| 5        | Skin controls, also called RFAOS | R roughness multiplier; G fuzz multiplier; B ambient occlusion; A reflectance multiplier |
| 4        | Wet surface                      | RGB tangent-space normal; A wetness mask                                                 |
| 8        | Detail normal + mask             | RGB repeatable tangent-space normal; A region mask painted in the model's main UV        |

Enter normal game paths such as `textures/MySkin/body_controls.dds`. Keep diffuse, base normal, face inputs and native slot 2 as they are unless you intend to edit those textures as part of your skin mod. The native skin texture in slot 2 controls transmission thickness: white is thin, black is thick. It is not the post-process SSS mask.

Save the NIF, reopen it, and check the property references and texture slots. Then install the copy and inspect it in-game. NifSkope does not preview this custom shader's full effect.

## Painting the textures

**Skin controls:** use linear data, not color-managed albedo. White leaves a material scalar unchanged; darker values reduce it. For example, roughness 0.6 multiplied by R=0.5 gives 0.3. B=1 leaves ambient lighting unoccluded. A multiplies Reflectance. Empty controls use white in all four channels.

**Detail:** RGB is a tileable normal. Alpha is a completely separate image laid out over the model's main UV: white allows full detail, black removes it, gray reduces it. Paint lips black to exclude pores. Changing Detail tiling changes the pore scale without moving that painted lip region. There is no detail AO channel.

For a painted mask use RGBA8 or BC7, retaining alpha. BC5 UNORM is suitable for a detail normal with full coverage: the engine reconstructs Z and assumes a white mask. BC5 cannot store your painted alpha. Supply mipmaps; inspect mask edges at a distance, since filtering and compression can soften them.

With the Detail slot empty, the material follows global detail texture, strength and tiling. Assigning a custom detail texture activates the material's own strength and tiling controls. **Enable detail** off disables both routes on that material; the global detail switch can disable all detail. A missing custom DDS is an error and will not silently switch to the global texture.

**Wet surface:** use a flat tangent normal `(0.5, 0.5, 1)` where no additional shape is needed. Alpha controls where wetness acts. Wet response scales the material's overall response. With no texture, the material uses a flat normal and full coverage.

### Pack channels without external scripts

Expand **Build a texture** in Skin Editor. For controls, choose equal-sized grayscale source images for R/G/B/A; the red channel of each image is used. Unselected channels use the displayed constants. For detail, enable **Detail normal + region mask**, select the normal and main-UV mask, then **Build and save DDS**. An empty mask writes white and discards any old detail alpha AO. Inputs must have equal dimensions; there is no automatic UV rescaling.

The tool accepts DDS, PNG, TGA, TIFF and BMP inputs up to 8192 × 8192, creates mipmaps and optionally compresses to BC7. Compression can take time. Assign the result with **Choose DDS**. **Texture channels and dependencies** shows individual channels and checks explicit texture resources.

## Customize a player or NPC

Choose **Player**, or select an NPC in the console and choose **Console selection**. Select the skin surface and **Customize this character part**. Check values to override; unchecked values inherit the installed material. Choose the face controls texture under **Skin controls**, then preview it. Under **Explicit surface targets**, select the intended surfaces.

Choose **Save and apply**. Settings save immediately, independently of the game save. You do not need to edit JSON, change an ESP or modify the installed model. Loading an older game does not undo changes. **Clear this part's customization** also saves immediately; **Undo last saved character edit** restores the previous committed edit.

The Player slot is shared across playthroughs using this configuration. Save named schemes for different looks and choose one when switching. Changing race, sex, head, body assignment, UV layout or base skin can pause affected settings. Returning to the original matching appearance resumes them. To keep settings on a changed appearance, preview first, then choose **Save for this appearance**.

Face, body, hands and feet are separate. First-person hands must be discovered and selected separately. Sources the editor cannot reliably identify remain preview-only. NPCs without a stable placed reference display **Apply for this session**; those settings end when the game session resets.

## Copy the player's settings to an NPC

1. Select the receiving NPC in the console and choose **Console selection** in Skin Editor.
2. Choose **Copy Skin settings from player**. Compatible parts save and apply in one action.
3. If a part is unavailable or a mapping needs attention, review the listed parts. Choose the source surface, enable **Preview mapped surfaces**, and inspect texture regions before confirming a manual mapping. Exclude parts you do not want, then **Save and apply selected parts**.

The NPC receives an independent copy. Later player edits do not change it. This copies Skin parameters and extension textures, including the player's inherited effective values, while keeping global detail as global. It does not copy face shape, color, main normal, thickness texture or current wetness.

For another source, choose **Use as copy source**, select the receiver, then **Apply copied scheme**. **Save character scheme** keeps a multi-part look for later; **Choose character scheme** opens the same preview and application workflow. Schemes use committed settings, so save edits before capturing them. Share referenced DDS files alongside the scheme.

Expand **Character configuration** to see the active file and saved targets. **Open configuration folder**, **Open configuration**, **Save configuration as**, **New configuration** and **Restore latest valid backup** manage storage without editing file contents. The default location is the CommunityShaders AdvancedSkin folder. Use separate files when you want separate mod setups; automatic mod-manager isolation is not assumed.

## Give a race such as UBE a default body material

Use the existing **RACE → skin ARMO → ARMA → model / TXST** assignment chain. First enable the material and set its parameters in the actual default-body NIFs. If all users of that NIF should share the texture, set its slot 5 directly.

If the race needs a texture variant, make an ESP patch in Creation Kit or xEdit using the corresponding ARMA skin texture set. In TXST, **TX02 maps to NIF slot 5** (Skin controls) and **TX05 maps to NIF slot 4** (Wet surface). Preserve the TXST's native diffuse, normal and skin inputs. Empty extension entries inherit the NIF's controls or wet texture. Detail slot 8 and numeric parameters stay in the NIF because TXST has no corresponding detail entry or parameter fields.

Check which records are shared. If the change is only for one race or body variant, copy the relevant shared records and point the intended race/skin chain at those copies. Include the male/female, hands, feet and first-person variants you actually support. An actor with its own skin override or equipment can use a different chain. UBE is handled through its records, without a special name-matching rule.

## Package and check

Distribute edited NIFs under `meshes`, new DDS files under `textures`, and an ESP only if you changed record assignments. **Save NIF with Skin textures** copies the explicitly assigned extension DDS files, preserving their resource paths. The original mod's native skin assets remain a dependency. A material preset NIF has no geometry and should not replace a body model.

Check face/body seams, lips and other excluded detail areas, near and distant views, dry and wet lighting, and first-person hands. After editing an installed DDS in another program, choose **Reload skin resources**. Check mod conflict winners if the saved result differs from preview. Save failures leave the draft intact and report conflicts or recovery files; do not keep overwriting a source another tool changed.

For existing old material exports, **Import old material file (one time)** can recover approximate scalar values and RFAOS assignment. It deliberately does not restore path matching, detail AO or old wet texture encoding. Preview the imported result and save it as a normal NIF or character customization.
