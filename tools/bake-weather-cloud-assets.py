"""Bake the weather cloud DDS files from a seed and analytic profiles.

Requires NumPy and SciPy. No external textures or presets are used.
"""

import argparse
from pathlib import Path
import struct

import numpy as np
from scipy.spatial import cKDTree


SIZE = 128


def dds(path, mips, dxgi_format, volume):
    first = mips[0]
    height, width = first.shape[-3:-1] if first.ndim == (4 if volume else 3) else first.shape[-2:]
    depth = first.shape[0] if volume else 1
    pixel_size = first.dtype.itemsize * (1 if first.ndim == (3 if volume else 2) else first.shape[-1])
    flags = 0x100F | (0x800000 if volume else 0) | (0x20000 if len(mips) > 1 else 0)
    caps = 0x1000 | (0x400008 if len(mips) > 1 else 0)
    header = (124, flags, height, width, width * pixel_size, depth if volume else 0, len(mips), *([0] * 11),
              32, 4, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0,
              caps, 0x200000 if volume else 0, 0, 0, 0, dxgi_format, 4 if volume else 3, 0, 1, 0)
    path.write_bytes(b"DDS " + struct.pack("<36I", *header) + b"".join(level.tobytes() for level in mips))


def mip_chain(noise):
    noise = np.clip(noise * 255, 0, 255)
    levels = []
    while True:
        levels.append(np.rint(noise).astype("u1"))
        size = noise.shape[0] // 2
        if size == 0:
            return levels
        noise = noise.reshape(size, 2, size, 2, size, 2, *noise.shape[3:]).mean(axis=(1, 3, 5))


def worley(rng):
    # The largest cells form the cloud lobes. Finer octaves erode their
    # contours, down to the sampling limit of the volume.
    axis = (np.arange(SIZE) + 0.5) / SIZE
    points = np.stack(np.meshgrid(axis, axis, axis, indexing="ij"), -1).reshape(-1, 3)
    noise = np.zeros(len(points), dtype="f4")
    weights = (1.0, 0.46, 0.35, 0.19, 0.105, 0.06, 0.025)
    for octave, weight in enumerate(weights):
        cells = 1 << octave
        lattice = np.indices((cells,) * 3).reshape(3, -1).T
        seeds = (lattice + rng.random(lattice.shape)) / cells
        distance, _ = cKDTree(seeds, boxsize=1.0).query(points, workers=-1)
        noise -= weight * np.square(distance * cells)
    noise = (noise - noise.mean()) * (0.161 / noise.std()) + 0.548
    return noise.reshape((SIZE,) * 3)


def curl(rng):
    # Differentiate a periodic random vector potential in Fourier space.
    # The curl is divergence-free before quantization.
    k = np.fft.fftfreq(SIZE) * SIZE
    kz, ky, kx = np.meshgrid(k, k, k, indexing="ij", sparse=True)
    radius_sq = kx * kx + ky * ky + kz * kz
    spectrum = (1.0 + radius_sq) ** -1.36 * np.exp(-(radius_sq / 45**2) ** 2)
    axes = (0, 1, 2)
    potential = np.fft.fftn(rng.standard_normal((SIZE, SIZE, SIZE, 3)), axes=axes)
    ax, ay, az = np.moveaxis(potential, -1, 0)
    components = (ky * az - kz * ay, kz * ax - kx * az, kx * ay - ky * ax)
    velocity = np.stack([np.fft.ifftn(1j * spectrum * c, axes=axes).real for c in components], axis=-1)
    result = np.ones((SIZE, SIZE, SIZE, 4), dtype="f4")
    result[..., :3] = 0.5 + velocity * (0.041 / velocity.std())
    return result


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def cloud_profiles():
    # Height above the nominal base, rise, start of the crown taper,
    # peak extinction, erosion width and noise wavelength at unit scale.
    # The low layer blends types continuously in a single noise lattice.
    # Varying the wavelength with type would shear the lattice across a front.
    low = np.array([
        (1700, .15, .36, .0015, .105, 2300),  # Stratus
        (2660, .09, .16, .012, .036, 2300),   # Cumulus
        (3660, .096, .17, .012, .034, 2300),  # Developing congestus
        (4680, .098, .87, .012, .034, 2300),  # Congestus
        (5700, .10, .74, .006, .033, 2300),   # Towering congestus
        (5700, .10, .71, .006, .033, 2300),   # Deep convective tower
    ])
    storm = np.array([
        (9700, .69, .70, .001, .46, 42000), # Diffuse trail
        (9700, .72, .73, .001, .045, 4100), # Anvil edge
        (9700, .10, .79, .002, .045, 3150), # Convective core
    ])
    atlas = np.zeros((2, SIZE + 1, 2 * SIZE, 4), dtype="<f2")
    for layer, entries in enumerate((low, storm)):
        height = np.linspace(0, entries[:, 0].max(), SIZE)
        for column, angle in enumerate(np.linspace(0, len(entries) - 1, SIZE), start=SIZE):
            i = int(angle)
            j = min(i + 1, len(entries) - 1)
            blend = angle - i
            depth, _, _, extinction, width, wavelength = (1 - blend) * entries[i] + blend * entries[j]
            h = height / depth
            curves = np.empty((2, SIZE))
            scales = np.empty_like(curves)
            for slot, key in enumerate((i, j)):
                _, rise, crown, _, _, _ = entries[key]
                if layer == 0:
                    # Wide lower domes give way to narrower, vertically sustained towers.
                    shoulder = (0.0, 0.0, 0.0, .54, .24, .20)[key]
                    crown_growth = (0.0, 0.0, 0.0, .03, .04, .21)[key]
                    fullness = .98 - shoulder * smooth(.12, .53, h) + crown_growth * smooth(.35, .67, h)
                    curves[slot] = smooth(.012, rise, h) * fullness * (1.0 - smooth(crown, .99, h))
                    scales[slot] = np.clip(h, 0.0, 1.0)
                elif key < 2:
                    onset = .47 if key == 0 else .37
                    curves[slot] = .98 * smooth(onset, rise, h) * (1.0 - smooth(crown, .81, h))
                    scales[slot] = np.clip(h, 0.0, 1.0)
                else:
                    core = .98 - .15 * smooth(.11, .36, h) + .21 * smooth(.32, .65, h)
                    curves[slot] = smooth(.016, rise, h) * core * (1.0 - smooth(crown, .99, h))
                    scales[slot] = 1.0
            coverage = np.rint(255 * np.clip((1 - blend) * curves[0] + blend * curves[1], 0, 1)) / 255
            density = (1 - blend) * scales[0] + blend * scales[1]
            atlas[layer, :SIZE, column, 0] = coverage
            atlas[layer, :SIZE, column, 1] = density
            last = np.flatnonzero(coverage)[-1]
            atlas[layer, SIZE, column] = (extinction, min((last + 2) / (SIZE - 1), 1), width, wavelength / 100)
        atlas[layer, 0, SIZE:SIZE + len(entries), 2] = entries[:, -1] / 100

    # The signed portion of low cloud type extends the stratus end of the
    # atlas into a thick, diffuse rain deck. Nonnegative coordinates retain
    # their original texels, including the storm noise metadata.
    h = np.linspace(0, 1, SIZE)
    deck = smooth(.13, .30, h) * (1 - smooth(.68, .98, h))
    fragments = smooth(.017, .064, h) * (1 - smooth(.083, .167, h))
    rain_cover = np.maximum(.98 * deck, .32 * fragments)
    rain_density = smooth(.13, .47, h) * (1 - .54 * smooth(.63, .98, h)) + .22 * fragments
    stratus = atlas[0, :, SIZE].astype(float)
    for column in range(SIZE):
        weight = min(1, (SIZE - column) / (SIZE - 1))
        coverage = (1 - weight) * stratus[:SIZE, 0] + weight * rain_cover
        atlas[0, :SIZE, column, 0] = np.rint(255 * coverage) / 255
        atlas[0, :SIZE, column, 1] = (1 - weight) * stratus[:SIZE, 1] + weight * rain_density
        support = min((np.flatnonzero(atlas[0, :SIZE, column, 0])[-1] + 2) / (SIZE - 1), 1)
        atlas[0, SIZE, column] = ((1 - weight) * stratus[SIZE, 0] + weight * .005, support,
                                  (1 - weight) * stratus[SIZE, 2] + weight * .15, 23)
    return atlas.reshape(2 * (SIZE + 1), 2 * SIZE, 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seed", type=int, default=77237, help="Random seed for the two noise volumes")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    dds(args.output / "CloudTypeProfiles.dds", [cloud_profiles()], 10, False)
    dds(args.output / "CloudWorley.dds", mip_chain(worley(rng)), 61, True)
    dds(args.output / "CloudCurl.dds", mip_chain(curl(rng)), 28, True)


if __name__ == "__main__":
    main()
