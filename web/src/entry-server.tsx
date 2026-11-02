import { renderToString } from 'react-dom/server';
import { App } from './App';

// The build renders the public pages to HTML with this (scripts/prerender.mjs)
export const render = (path: string) => renderToString(<App path={path} />);
