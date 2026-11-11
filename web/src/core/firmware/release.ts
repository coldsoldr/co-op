/**
 * The coop release the Build firmware tab makes. Written from firmware/tools/make_web_delta.py's output
 * for the package that passed its gates and was flashed (firmware/docs/device-tests.md).
 */
export const COOP_RELEASE = {
  tag: 'e10',
  /** The name the built package downloads as. */
  fileName: 'op1_246_coop_e10.op1',
  /** public/firmware/<delta>, served with the site. */
  delta: 'firmware/coop-e10.delta',
  deltaSha256: '57e730f8596ddc72f76dfe0e4ca0de048c955d1177861b5d06b679d9a0e293ea',
  /** R246's OP1_vdk.ldr, from TE's op1_246.op1. */
  stockLdrSha256: 'f79d299f6f9019c998013a4176223a6a1bc999a2d8c54451346861f6ea455c74',
  /** The coop OP1_vdk.ldr the delta rebuilds: byte for byte the one in the tested package. */
  coopLdrSha256: '502a7037c3bebfbc2ad67f868cd14e41b924fd7e69f22d3adc5032a051687668',
  /** The owner's tested package; a browser build packs the same files, so its own hash differs. */
  referencePackageSha256: '23367c931e4ec33dfabbe00c44f2035bd3f505a53ed2f4ab2b7b359e722e23f4',
} as const;
