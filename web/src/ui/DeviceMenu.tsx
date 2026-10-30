import { useId, useRef } from 'react';
import { DEVICES, type SiteDevice } from './device-choice';

interface DeviceMenuProps {
  device: SiteDevice;
  onChoose: (device: SiteDevice) => void;
}

/** The logo, which opens the list of devices the site can show. */
export function DeviceMenu({ device, onChoose }: DeviceMenuProps) {
  const menuId = useId();
  const button = useRef<HTMLButtonElement>(null);
  const menu = useRef<HTMLDivElement>(null);
  const label = DEVICES.find((d) => d.id === device)?.label;

  /** The menu sits in the top layer and opens below the logo. */
  function place(): void {
    const logo = button.current?.getBoundingClientRect();
    if (!logo || !menu.current) return;
    menu.current.style.top = `${logo.bottom + 8}px`;
    menu.current.style.left = `${logo.left}px`;
  }

  return (
    <>
      <h1>
        <button
          ref={button}
          type="button"
          className="device-button"
          popoverTarget={menuId}
          aria-label={`Choose a device. co-op patch librarian for ${label}`}
          onClick={place}
        >
          <span className="logo" aria-hidden="true">
            <span className="blue" />
            <span className="green" />
            <span className="grey" />
            <span className="orange" />
          </span>
          co-op
          <svg aria-hidden="true" viewBox="0 0 24 24">
            <path d="M6 9l6 6 6-6" />
          </svg>
        </button>
      </h1>
      <div ref={menu} id={menuId} popover="auto" className="menu device-menu">
        {DEVICES.map(({ id, label: name }) => (
          <button
            key={name}
            type="button"
            disabled={id === null}
            aria-pressed={id === device}
            onClick={() => {
              menu.current?.hidePopover();
              if (id) onChoose(id);
            }}
          >
            {name}
          </button>
        ))}
      </div>
    </>
  );
}
