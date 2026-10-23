// Clipboard text is deliberately reviewed before it can become multiple commands.
export function needsPasteConfirmation(text: string): boolean {
  return /[\r\n\x00-\x08\x0b-\x1f\x7f]/u.test(text);
}
export function remoteJoin(directory: string, name: string): string {
  if (!name || name === "." || name === ".." || /[/\\\0]/u.test(name))
    throw new Error("Invalid remote filename");
  return `${directory.replace(/\/$/u, "")}/${name}`;
}
export function boundedFontSize(value: number): number {
  return Math.max(
    11,
    Math.min(24, Number.isFinite(value) ? Math.round(value) : 14),
  );
}
