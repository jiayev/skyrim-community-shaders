# Advanced Skin Artist Guide

Enable Advanced Skin in the feature settings and it applies to all compatible skin immediately, using the **Common skin parameters**. You only need the rest of this guide when a skin should look different from the common settings, or when you have painted maps for it.

These instructions describe the implementation in this branch. It builds but has not yet been validated in the game; treat unexpected results as possible bugs and report them.

## The one rule

**A material belongs to a skin texture.** Not to a mesh, not to an outfit, not to a character.

For every skin diffuse texture you can put a small file beside it:

```
textures/actors/character/female/femalebody_1.dds          <- your skin
textures/actors/character/female/femalebody_1.csskin.json  <- its material
```

Everything that shows that texture uses that material:

-   Every character and NPC using the skin.
-   Bare arms, legs and midriffs inside armor and clothing. The game puts the wearer's skin texture on those shapes, so they match the naked body automatically. You never set up a material per outfit.
-   First-person arms.

And anything that shows a different texture uses a different material. Khajiit and humans may share a body mesh, but they have different textures, so they have separate materials. If the player changes race, the head starts showing another texture and picks up that texture's material by itself.

You do not edit meshes, you do not need an ESP, and you do not need to know which mesh an outfit uses.

## Make a material in the game

1. Open **CS Editor → Skin Editor → Skin texture materials**.
2. Choose **Player**, or select an NPC in the console and choose **Console selection**.
3. The table lists every skin texture this character is showing, where each one appears (head, body, hands, feet, an outfit's name, first person), and whether it already has a material file. Select the texture you want to work on.
4. Adjust the material. Tick **Override** beside a value to store it in this material; unticked values keep following the common settings, so a material only needs the things that make this skin different. The categories are **Textures**, **Dual specular lobes**, **Fuzz and edges**, **Transmission and SSS**, **Detail normals** and **Wet response**.
5. Watch the result. While the editor is open, your draft is shown on **everything that uses this texture**, on every loaded character, exactly as it will look after saving. Use **Test a specific wetness level** to check the wet look.
6. Choose **Save material file**. It is written beside the texture under the game's Data folder (with Mod Organizer this lands in Overwrite). To write straight into your mod instead, choose **Write into a mod folder...** once and pick your mod's folder; it is remembered with the feature's settings and also receives textures built by the texture tools.

Texture inputs are picked with **Choose DDS**, which browses the Data folder as the game sees it — with Mod Organizer, the merged view of all your mods — and stores a game path such as `textures/myskin/body_rfaos.dds`. You never deal with where a file physically lives, and the material keeps working when the texture is later packed into a BSA. Textures inside a BSA are not listed; type their path under **Resource path** instead.

A skin set usually has several textures: head, body, hands and often feet, and separate ones for each sex and for beast races. Each needs its own material file. To give them the same numeric values, save one, select the next texture, and choose **Copy values from another material file...**. Painted maps are not copied, because they only fit the texture they were painted for.

**Undo**, **Redo** and **Discard draft** are at the bottom. Selecting another texture with unsaved changes asks first.

## Write a material by hand

The file is plain JSON. Include only what differs from the common settings.

```json
{
    "format": "CommunityShaders.SkinMaterial",
    "version": 1,
    "parameters": {
        "Roughness": 0.62,
        "SecondaryRoughness": 0.3,
        "Fuzz": 0.8
    },
    "textures": {
        "controls": "textures/myskin/femalebody_rfaos.dds",
        "wet": "textures/myskin/femalebody_wet.dds",
        "detail": "textures/myskin/pores_n.dds"
    }
}
```

The [parameter table](advanced-skin-materials.md#parameters) lists every name and range. `//` comments are allowed.

If the file has a mistake, the whole file is ignored and the skin uses the common settings. Nothing is half-applied. The reason is in the Community Shaders log (search for `[Advanced Skin]`) and in the Skin Editor when you select the texture. Typical causes are a missing comma, a misspelled parameter, or a value outside its range.

After editing a file or a DDS outside the game, choose **Reload skin resources**.

## Painting the textures

All three are optional. Paint them in the UV layout of the diffuse texture they belong to.

**Skin controls (`controls`, also called RFAOS)** — linear data.

| Channel | Meaning                               |
| ------- | ------------------------------------- |
| R       | Roughness                             |
| G       | Fuzz multiplier                       |
| B       | Ambient occlusion; 1 means unoccluded |
| A       | Specular                              |

Without this texture, **Primary Roughness** and **Secondary Roughness** set the two highlights directly and reflection intensity uses Reflectance (F0). With it, R drives both highlights through the two **Physical Roughness Multipliers**: R = 0.5 with multipliers 1.3 and 0.75 gives 0.65 and 0.375 before edge roughness. Specular becomes `0.08 × A × Physical Specular Multiplier`. **Secondary Specular Strength** mixes the two highlights in both cases.

**Detail (`detail`)** — RGB is a tileable tangent-space normal such as pores. Alpha is a separate image in the diffuse's UV layout: white allows full detail, black removes it. Paint the lips black to keep pores off them. Changing **Detail tiling** changes the pore scale without moving the painted regions. With no detail texture, the common detail texture is used; **Body Tiling Multiplier** scales it outside the head.

**Wet surface (`wet`)** — RGB is a tangent-space normal for the wet layer; use flat `(0.5, 0.5, 1)` where no extra shape is needed. Alpha controls where wetness acts. With no texture, wetness is flat and covers everything.

The game's own skin texture (`_sk`) controls how much light passes through thin parts: white is thin, black is thick. That is separate from **SSS strength**, which controls the post-process scattering.

Formats: RGBA8 or BC7 for anything with a painted alpha. BC5 UNORM works for a detail normal with full coverage only; it cannot store the mask. Supply mipmaps, and check mask edges from a distance.

### Pack channels without external tools

Open the **Texture tools** workspace. For controls, choose equal-sized grayscale images for R, G, B and A; unselected channels use the displayed constants. For detail, enable **Detail normal + region mask** and choose the normal and the mask. Source images can come from anywhere on disk. Under **Output**, give the game path of the result; the editor suggests one beside the skin texture you are editing. **Build and save DDS** writes linear data with mipmaps, optionally as BC7, into the output mod folder. **Use this texture in the current material** assigns the result to the draft you were editing. Inputs can be DDS, PNG, TGA, TIFF or BMP up to 8192 × 8192 and must have equal dimensions.

## Adjust one character

Use this for a player or follower who should look a little different from everyone else with the same skin — oilier, less fuzz, a personal detail mask.

1. Open **Skin Editor → This character** and choose **Player** or **Console selection**.
2. Choose the scope:
    - **All skin of this character** — numeric values only, applied to head, body, hands and feet alike.
    - **Only this skin texture** — values and textures for the texture selected in the table.
3. Adjust, then choose **Save character file**.

The adjustments are saved as a small JSON file for that character in `Data/SKSE/Plugins/CommunityShaders/AdvancedSkin/Characters`. It applies in every save, to every actor using that NPC record, and you can edit it by hand, share it, or ship it with a follower mod. The [file format](advanced-skin-materials.md#character-files) is described in the technical reference. After editing it outside the game, choose **Reload skin resources**.

The player file applies to whichever character you are playing. To keep several looks, save copies and swap them in, or load one into the draft with **Load from another character file...**.

If the character stops showing a texture — for example the player changes race — the adjustments for that texture are kept under **Adjustments for textures not currently shown**. They return if the texture returns. From there you can remove them, or copy their numeric values to the texture selected now.

Under **Files and sharing**, **Load from another character file...** copies another character's adjustments into this draft, and **Save a copy...** writes one for sharing. Texture-specific entries take effect wherever the same texture is shown.

## Package and check

Ship each `.csskin.json` beside its diffuse texture, plus the DDS files it refers to, loose or in a BSA. No ESP is needed. Because the material file sits next to the texture and follows the same priority rules, a mod that overwrites your texture can also ship its own material for it.

When a material was saved from the editor, it records which version of the diffuse it was made for. If another mod later replaces the texture but your material file still wins, the editor shows a warning when the texture is selected, since painted masks may no longer line up.

Check face and body seams, lips and other areas excluded from detail, near and distant views, dry and wet lighting, first-person hands, and at least one outfit with exposed skin.

## Troubleshooting

| Symptom                                               | Likely cause                                                                                                       |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| The material does nothing                             | The file name does not match the diffuse the character actually shows. Check the path in the editor's table.       |
| The table says **Invalid material file**              | A mistake in the JSON. Select the texture to see the reason.                                                       |
| Saved, but the game shows something else after reload | Another mod provides the same file with higher priority, or the file was saved to a folder the game does not load. |
| A texture input is ignored                            | The DDS is missing or in an unsupported format; the message appears in the log.                                    |
| The table says **Unidentified texture**               | The surface's diffuse is not an ordinary DDS file, so it cannot have a material. It keeps the common settings.     |
