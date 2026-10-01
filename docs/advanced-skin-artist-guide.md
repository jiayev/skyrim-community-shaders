# Advanced Skin Artist Guide

Enable Advanced Skin in the feature settings: it applies to all compatible skin immediately. Adjust **Common skin parameters** for the shared appearance. Edit a NIF or character only when it needs different values or painted controls. A body, face, hands and feet may use separate files and materials.

You can work entirely in the game's **CS Editor → Skin Editor**, or edit the NIF in **NifSkope**. Both routes produce ordinary NIF and DDS files. ESP patches are only needed when changing which assets a race, armor or actor uses. You do not need Python or material JSON.

These instructions describe the implementation in this branch. The complete NifSkope/editor/game workflow has not yet been validated in-game. Use a test installation containing this implementation; older Advanced Skin releases use different data.

## Route 1: use the in-game editor

Start in **CS Editor → Skin Editor → NIF authoring**. The header keeps the current file, material, unsaved state and temporary preview state visible. The bottom bar provides undo/redo, discard and step navigation.

1. **Select target:** use **Open NIF** and choose its **NIF material**. **New material template** creates a reusable material without a mesh. Archive paths are under **Game resource / archive input**.
2. **Adjust material:** choose **Textures**, **Dual specular lobes**, **Fuzz and edges**, **Transmission and SSS**, **Detail normals** or **Wet response**. Check **Override** only beside values you want this material to change. Other values continue to inherit the common settings. Texture slots use DDS pickers. Wide panels show a category sidebar; narrow panels use a category selector.
3. **Review and save:** choose Player, First person or Console selection, then a **Preview surface**. This selection keeps the NIF draft open. Preview starts automatically when a loaded surface is selected and updates as you edit. Use **Test a specific wetness level** if needed; otherwise the character’s current wetness is used. A preview target is optional for NIF authoring.
4. Choose **Save NIF as...**. For distribution with new DDS files, use **Save NIF with Skin textures...** and review the output list. The save section explains whether the destination is a shared NIF or one character's external settings.

**Texture tools** and **Configuration** have their own workspaces. Visiting them preserves both your draft and its live preview. Closing the CS Editor ends the temporary preview; reopening resumes it. Saving keeps your edits, while discarding restores the draft’s starting values. **Return to material draft** resumes editing. Choosing a different file or character asks before discarding unsaved edits. If you choose **Keep editing**, you return to the original draft. Common Skin settings remain in the feature's own settings panel.

If several shapes use the same material, its changes affect them together. Expand **Shapes using this material** and choose **Make independent** for a shape that needs different settings. This prepares a separate material; save the NIF to commit it.

A saved NIF does not rebuild a character that is already loaded. Preview lets you inspect the draft immediately; rebuild/reload the model normally to check the installed asset. Do not assume a working preview proves the correct source file won your mod conflicts.

Use **New material template** to create a reusable preset without geometry. Under **Material presets**, save or import a material-only NIF and select which settings or textures to copy. This preset does not replace a body mesh.

**Batch asset output** applies the current draft to an explicit list of NIFs. Choose the material and output file for every body, hand, foot, face or first-person variant. Review the list before publishing. BodySlide or FaceGen regeneration may overwrite these outputs; apply the material again to regenerated files or maintain it in the relevant source workflow.

## Route 2: edit the NIF in NifSkope

### Select the existing skin material

Work on a copy of the final Skyrim SE NIF. Select the skin shape and follow its `BSLightingShaderProperty` link. Its native shader should already be **Face Tint (4)** or **Skin Tint (5)**. Keep its native flags, UV settings and ordinary skin textures. This does not convert hair or clothing into skin.

The settings below belong in the **shader property's Extra Data List**. They do not belong on the root, the shape or a bone.

### Add a material override and parameters

Insert a `NiIntegerExtraData` block, set its **Name** to `CS_SkinVersion` and **Integer Data** to `1`, then add a reference to it in the shader property's Extra Data List. This number identifies the stored override format. Ordinary skin already uses the common Advanced Skin settings without this block.

For each parameter you want to change, insert the listed extra-data block, enter its Name and Float Data or Integer Data, and reference it from that same list. Resize the property's list when adding references. Use one block per field; edit an existing field instead of duplicating it. NifSkope menu wording varies, but these block types and field names are the file data.

You can also open the supplied `meshes/CS/Skin/SkinMaterial.nif` alongside your model and copy its typed extra-data blocks. Link the copied blocks to your existing shader property. Keep the target property's native settings.

Only add fields you want this material to override. For example:

| Control                                    | Extra-data name                          | Example value |
| ------------------------------------------ | ---------------------------------------- | ------------: |
| Primary roughness, without RFAOS           | CS_SkinRoughness                         |           0.7 |
| Secondary roughness, without RFAOS         | CS_SkinSecondaryRoughness                |          0.35 |
| Mix of the second highlight                | CS_SkinSecondarySpecularStrength         |          0.15 |
| Primary roughness multiplier, with RFAOS   | CS_SkinPhysicalMainRoughnessMultiplier   |           1.3 |
| Secondary roughness multiplier, with RFAOS | CS_SkinPhysicalSecondRoughnessMultiplier |          0.75 |

These are NiFloatExtraData fields. Delete an override field to follow the common value again. See the [complete parameter table](advanced-skin-materials.md#asset-and-assignment-model) for reflectance, color, fuzz, transmission, detail and wet response. Both the common panel and material editor expose these controls. SSS and transmission remain separate.

### Assign ordinary texture slots

Follow the property's **Texture Set** link to `BSShaderTextureSet`. Expand its nine-entry Textures array. Numbers here start at zero; slot 5 is the sixth entry.

| NIF slot | Optional texture                 | Paint these channels                                                              |
| -------- | -------------------------------- | --------------------------------------------------------------------------------- |
| 5        | Skin controls, also called RFAOS | R roughness; G fuzz multiplier; B ambient occlusion; A specular                   |
| 4        | Wet surface                      | RGB tangent-space normal; A wetness mask                                          |
| 8        | Detail normal + mask             | RGB repeatable tangent-space normal; A region mask painted in the model's main UV |

Enter normal game paths such as `textures/MySkin/body_controls.dds`. Keep diffuse, base normal, face inputs and native slot 2 as they are unless you intend to edit those textures as part of your skin mod. The native skin texture in slot 2 controls transmission thickness: white is thin, black is thick. It is not the post-process SSS mask.

Save the NIF, reopen it, and check the property references and texture slots. Then install the copy and inspect it in-game. NifSkope does not preview this custom shader's full effect.

## Painting the textures

**Skin controls (RFAOS):** use linear data. With no RFAOS texture, adjust **Primary Roughness** and **Secondary Roughness** independently; the original specular texture multiplier still affects the primary layer. With RFAOS, its R channel drives both layers, each with its own **Physical Roughness Multiplier**. For example, R=0.5 with multipliers 1.3 and 0.75 gives 0.65 and 0.375 before edge roughness. **Secondary Specular Strength** mixes the two highlights. G masks fuzz; B=1 leaves ambient lighting unoccluded. A uses the original specular mapping: `0.08 × A × Physical Specular Multiplier`. Without RFAOS, reflection intensity uses Reflectance (F0).

**Detail:** RGB is a tileable normal. Alpha is a completely separate image laid out over the model's main UV: white allows full detail, black removes it, gray reduces it. Paint lips black to exclude pores. Changing Detail tiling changes the pore scale without moving that painted lip region. There is no detail AO channel.

For a painted mask use RGBA8 or BC7, retaining alpha. BC5 UNORM is suitable for a detail normal with full coverage: the engine reconstructs Z and assumes a white mask. BC5 cannot store your painted alpha. Supply mipmaps; inspect mask edges at a distance, since filtering and compression can soften them.

With the Detail slot empty, the material uses the common detail texture. Enable, strength and tiling inherit the common settings unless you explicitly override them, whether the texture is common or custom. Body tiling multiplies repeats outside the head. A missing custom DDS is an error and will not silently switch to the common texture.

**Wet surface:** use a flat tangent normal `(0.5, 0.5, 1)` where no additional shape is needed. Alpha controls where wetness acts. Wet response scales the material's overall response. With no texture, the material uses a flat normal and full coverage.

### Pack channels without external scripts

Open the **Texture tools** workspace in Skin Editor. For controls, choose equal-sized grayscale source images for R/G/B/A; the red channel of each image is used. Unselected channels use the displayed constants. For detail, enable **Detail normal + region mask**, select the normal and main-UV mask, then **Build and save DDS**. An empty mask writes white and discards any old detail alpha AO. Inputs must have equal dimensions; there is no automatic UV rescaling.

The tool accepts DDS, PNG, TGA, TIFF and BMP inputs up to 8192 × 8192, creates mipmaps and optionally compresses to BC7. Compression can take time. Use **Use this texture in the current material** to assign the result and return to the draft, or use **Choose DDS** later. **Texture channels and dependencies** shows individual channels and checks explicit texture resources.

## Customize a player or NPC

Open **Character customization → Select target**. Choose **Player**, or select an NPC in the console and choose **Console selection**. Select **Surface to customize** and review **Explicit surface targets**, including first-person and third-person variants when loaded. Continue to **Adjust material**. Check values to override; unchecked values inherit the installed material. Choose the face controls texture under **Skin controls**, then preview it. Under **Explicit surface targets**, select the intended surfaces.

Choose **Save and apply**. Settings save immediately, independently of the game save. You do not need to edit JSON, change an ESP or modify the installed model. Loading an older game does not undo changes. **Clear this part's customization** also saves immediately; **Undo last saved character edit** restores the previous committed edit.

The Player slot is shared across playthroughs using this configuration. Save named schemes for different looks and choose one when switching. Changing race, sex, head, body assignment, UV layout or base skin can pause affected settings. Returning to the original matching appearance resumes them. To keep settings on a changed appearance, preview first, then choose **Save for this appearance**.

Face, body, hands and feet are separate. First-person hands must be discovered and selected separately. Sources the editor cannot reliably identify remain preview-only. NPCs without a stable placed reference display **Apply for this session**; those settings end when the game session resets.

## Copy the player's settings to an NPC

1. In **Character customization → Select target**, select the receiving NPC in the console and choose **Console selection** in Skin Editor.
2. Choose **Copy or reuse a character scheme → Copy Skin settings from player**. Compatible parts save and apply in one action.
3. If a part is unavailable or a mapping needs attention, review the listed parts. Choose the source surface and inspect the automatic preview before confirming a manual texture mapping. Exclude parts you do not want, then **Save and apply selected parts**.

The NPC receives an independent copy. Later player edits do not change it. This copies Skin parameters and extension textures, including the player's inherited effective values, while keeping global detail as global. It does not copy face shape, color, main normal, thickness texture or current wetness.

For another source, choose **Use as copy source**, select the receiver, then **Apply copied scheme**. **Save character scheme** keeps a multi-part look for later; **Choose character scheme** opens the same preview and application workflow. Schemes use committed settings, so save edits before capturing them. Share referenced DDS files alongside the scheme.

Open the **Configuration** workspace to see the active file and saved targets. **Open configuration folder**, **Open configuration**, **Save configuration as**, **New configuration** and **Restore latest valid backup** manage storage without editing file contents. The default location is the CommunityShaders AdvancedSkin folder. Use separate files when you want separate mod setups; automatic mod-manager isolation is not assumed.

## Give a race such as UBE a default body material

Use the existing **RACE → skin ARMO → ARMA → model / TXST** assignment chain. First customize the material and set only its required overrides in the actual default-body NIFs. If all users of that NIF should share the texture, set its slot 5 directly.

If the race needs a texture variant, make an ESP patch in Creation Kit or xEdit using the corresponding ARMA skin texture set. In TXST, **TX02 maps to NIF slot 5** (Skin controls) and **TX05 maps to NIF slot 4** (Wet surface). Preserve the TXST's native diffuse, normal and skin inputs. Empty extension entries inherit the NIF's controls or wet texture. Detail slot 8 and numeric parameters stay in the NIF because TXST has no corresponding detail entry or parameter fields.

Check which records are shared. If the change is only for one race or body variant, copy the relevant shared records and point the intended race/skin chain at those copies. Include the male/female, hands, feet and first-person variants you actually support. An actor with its own skin override or equipment can use a different chain. UBE is handled through its records, without a special name-matching rule.

## Package and check

Distribute edited NIFs under `meshes`, new DDS files under `textures`, and an ESP only if you changed record assignments. **Save NIF with Skin textures** copies the explicitly assigned extension DDS files, preserving their resource paths. The original mod's native skin assets remain a dependency. A material preset NIF has no geometry and should not replace a body model.

Check face/body seams, lips and other excluded detail areas, near and distant views, dry and wet lighting, and first-person hands. After editing an installed DDS in another program, choose **Reload skin resources**. Check mod conflict winners if the saved result differs from preview. When saving is unavailable, the save page shows the reason. Configuration errors do not disable Save. If the file was deleted, Save recreates it from the currently loaded settings and your edits. External changes to other characters or unedited parts are kept automatically. If the same parts changed on disk, or the existing file cannot be read, the editor offers **Back up disk file and save my edits** or **Keep editing**, without discarding the draft or requiring a file picker. An unreadable file is preserved as a backup before replacement; settings that could not be loaded are only retained in that backup. Save failures leave the draft intact and report conflicts or recovery files; do not keep overwriting a source another tool changed.

For existing old material exports, **Import old material file (one time)** can recover approximate scalar values and RFAOS assignment. It deliberately does not restore path matching, detail AO or old wet texture encoding. Preview the imported result and save it as a normal NIF or character customization.
