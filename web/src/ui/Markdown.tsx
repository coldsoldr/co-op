import { Fragment, useMemo, type ReactNode } from 'react';
import { parseMarkdown, type Align, type Block, type Inline } from '../core/plugins/markdown';

interface MarkdownProps {
  source: string;
  /** The plugin folder's URL, with a trailing slash: relative links and images resolve into it. */
  folderUrl: string;
  /** Added to every heading level (capped at h6), so a manual's `#` fits under the page's own headings. */
  headingOffset?: number;
}

/**
 * Renders a plugin manual. The parser (core/plugins/markdown) has already dropped unsafe links and
 * images; everything here is a React element or text, so no part of the manual is ever HTML.
 */
export function Markdown({ source, folderUrl, headingOffset = 0 }: MarkdownProps) {
  const blocks = useMemo(() => parseMarkdown(source, folderUrl), [source, folderUrl]);
  return <div className="markdown">{renderBlocks(blocks, headingOffset)}</div>;
}

function renderBlocks(blocks: readonly Block[], offset: number, tight = false): ReactNode[] {
  return blocks.map((block, i) => {
    // A tight list's paragraphs render without <p>, as in most Markdown renderers.
    if (tight && block.type === 'paragraph') return <Fragment key={i}>{renderInlines(block.children)}</Fragment>;
    return <Fragment key={i}>{renderBlock(block, offset)}</Fragment>;
  });
}

function renderBlock(block: Block, offset: number): ReactNode {
  switch (block.type) {
    case 'heading': {
      const Heading = `h${Math.min(6, block.level + offset)}` as 'h1';
      return <Heading>{renderInlines(block.children)}</Heading>;
    }
    case 'paragraph':
      return <p>{renderInlines(block.children)}</p>;
    case 'code':
      return (
        <pre>
          <code>{block.text}</code>
        </pre>
      );
    case 'list': {
      const items = block.items.map((item, i) => <li key={i}>{renderBlocks(item, offset, block.tight)}</li>);
      return block.ordered ? <ol start={block.start === 1 ? undefined : block.start}>{items}</ol> : <ul>{items}</ul>;
    }
    case 'table':
      return (
        <table className="library manual-table">
          <thead>
            <tr>
              {block.head.map((cell, c) => (
                <th key={c} scope="col" style={alignStyle(block.align[c])}>
                  {renderInlines(cell)}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {block.rows.map((row, r) => (
              <tr key={r}>
                {row.map((cell, c) => (
                  <td key={c} style={alignStyle(block.align[c])}>
                    {renderInlines(cell)}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      );
    case 'quote':
      return <blockquote>{renderBlocks(block.children, offset)}</blockquote>;
    case 'rule':
      return <hr />;
  }
}

const alignStyle = (align: Align | undefined) => (align ? { textAlign: align } : undefined);

function renderInlines(inlines: readonly Inline[]): ReactNode[] {
  return inlines.map((node, i) => {
    switch (node.type) {
      case 'text':
        return <Fragment key={i}>{node.text}</Fragment>;
      case 'code':
        return <code key={i}>{node.text}</code>;
      case 'em':
        return <em key={i}>{renderInlines(node.children)}</em>;
      case 'strong':
        return <strong key={i}>{renderInlines(node.children)}</strong>;
      case 'link':
        return node.external ? (
          <a key={i} href={node.href} target="_blank" rel="noopener noreferrer">
            {renderInlines(node.children)}
          </a>
        ) : (
          <a key={i} href={node.href}>
            {renderInlines(node.children)}
          </a>
        );
      case 'image':
        return <img key={i} src={node.src} alt={node.alt} loading="lazy" />;
      case 'break':
        return <br key={i} />;
    }
  });
}
