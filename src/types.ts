export interface Host {
  id: string;
  name: string;
  hostname: string;
  port: number;
  username: string;
  vault: string;
  auth: "password" | "key" | "certificate";
  keyPath: string;
  certificatePath: string;
  favorite: boolean;
}
export interface Settings {
  theme: "nord" | "midnight";
  fontSize: number;
  checkUpdates: boolean;
}
export interface Snippet {
  id: string;
  name: string;
  command: string;
}
export interface Snapshot {
  hosts: Host[];
  settings: Settings;
  snippets: Snippet[];
  version: string;
  knownHosts?: Record<string, string>;
}
export interface TerminalEvent {
  id: string;
  kind: "data" | "closed" | "error";
  data: number[];
  message: string;
}
export interface RemoteEntry {
  name: string;
  size: number;
  directory: boolean;
}
