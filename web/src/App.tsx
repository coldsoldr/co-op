import { Inspector } from './ui/Inspector';

export function App() {
  return (
    <main>
      <h1>co-op patch librarian</h1>
      <p>Early build. Drop an OP-1 patch below to check that we read it correctly.</p>
      <Inspector />
    </main>
  );
}
