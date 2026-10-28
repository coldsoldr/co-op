import { useEffect, useId, useRef, type ReactNode } from 'react';

interface DialogProps {
  title: string;
  /** Called when the dialog is dismissed (Escape, or a Cancel button calling it). */
  onClose: () => void;
  children: ReactNode;
}

/** A modal native <dialog>, shown while mounted. Escape closes it through onClose. */
export function Dialog({ title, onClose, children }: DialogProps) {
  const ref = useRef<HTMLDialogElement>(null);
  const titleId = useId();

  useEffect(() => {
    const dialog = ref.current;
    if (dialog && !dialog.open) dialog.showModal();
    return () => {
      if (dialog?.open) dialog.close();
    };
  }, []);

  return (
    <dialog
      ref={ref}
      aria-labelledby={titleId}
      onCancel={(event) => {
        event.preventDefault();
        onClose();
      }}
    >
      <h3 id={titleId}>{title}</h3>
      {children}
    </dialog>
  );
}

interface ConfirmProps {
  title: string;
  /** What will happen, shown above the list of files. */
  message: string;
  files: readonly string[];
  confirmLabel: string;
  onConfirm: () => void;
  onCancel: () => void;
}

/** A confirmation for a destructive action that names every file it touches. */
export function ConfirmDialog({ title, message, files, confirmLabel, onConfirm, onCancel }: ConfirmProps) {
  return (
    <Dialog title={title} onClose={onCancel}>
      <p>{message}</p>
      <ul className="file-list">
        {files.map((file) => (
          <li key={file}>
            <code>{file}</code>
          </li>
        ))}
      </ul>
      <div className="dialog-buttons">
        <button type="button" onClick={onCancel} autoFocus>
          Cancel
        </button>
        <button type="button" className="danger" onClick={onConfirm}>
          {confirmLabel}
        </button>
      </div>
    </Dialog>
  );
}
