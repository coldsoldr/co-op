/** The op1.fun account email and API token, sent as X-User-Email and X-User-Token. */
export interface Op1FunCredentials {
  email: string;
  token: string;
}

export interface Op1FunPatch {
  id: string;
  name: string;
  description: string;
  /** '' or one of the CC values in OP1FUN_LICENSES. */
  license: string;
  public: boolean;
  /** The op1.fun patch type as given by the API (for example 'drum' or 'synth'), or ''. */
  patchType: string;
  /** The AIFF file (S3), or ''. */
  fileUrl: string;
  /** The MP3 preview (S3), or ''. */
  previewUrl: string;
  /** The patch page on op1.fun (links.self), or ''. */
  selfUrl: string;
  userId: string;
}

export interface Op1FunPack {
  id: string;
  name: string;
  description: string;
  public: boolean;
  patchIds: string[];
  downloadUrl: string;
}

export interface Op1FunUser {
  id: string;
  username: string;
  patchIds: string[];
  packIds: string[];
}

export interface Op1FunUpload {
  name: string;
  description: string;
  license: string;
  public: boolean;
  /** The raw AIFF bytes. */
  bytes: Uint8Array;
}

export const OP1FUN_LICENSES = ['', 'CC BY', 'CC BY-SA', 'CC BY-NC', 'CC BY-ND'] as const;

export class Op1FunError extends Error {
  override name = 'Op1FunError';
}

/** op1.fun rejected the email/token (HTTP 401). */
export class Op1FunAuthError extends Op1FunError {
  override name = 'Op1FunAuthError';
}
