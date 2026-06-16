This directory holds all your assets in your project that can be
processed directly by ngdevkit. For example:

- Graphics assets like spritesheets or tileset in gif or png format.
  Prefer gif format, as it is always parsed appropriately by ngdevkit,
  no matter your environment.

- Sound FX, one file per FX, in .wav format. The file should either be
  a mono 18500Hz for ADPCM-A channel, or any other frequency for
  ADPCM-B channel. No compression is currently supported.

- Furnace module in .fur format. This format is directly compiled into
  NSS bytecode in Z80 assembler for ngdevkit.

If your assets come into different formats (for example FX in .mp3),
they must go into the ./setup directory and get pre-processed prior to
building your project.
