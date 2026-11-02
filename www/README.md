# Foundry Local Website

Official marketing website for Foundry Local - Microsoft's on-device AI inference platform.

## About

This repository contains the source code for the Foundry Local website, built with **SvelteKit** and **TailwindCSS**. The site provides:

- Product overview and features
- Interactive models catalog with search and filtering
- Links to official documentation on Microsoft Learn
- Dark mode support
- Responsive design for all devices

## Technology Stack

- **Runtime**: Bun (recommended) or Node.js 18+
- **Framework**: SvelteKit 2.x
- **Styling**: TailwindCSS with custom components from shadcn-svelte
- **Language**: TypeScript
- **Build Tool**: Vite 6.x
- **Deployment**: Vercel (via adapter-vercel)

## Development

### Prerequisites

- **Bun** (recommended) - [Install Bun](https://bun.sh)
- **Node.js 18+** and npm (alternative)

### Setup

```bash
# Install dependencies
bun install        # Using Bun (recommended)
# or
npm install        # Using npm

# Start development server
bun run dev        # Using Bun
# or
npm run dev        # Using npm

# Open http://localhost:5173
```

### Available Scripts

| Command | Description |
|---------|-------------|
| `bun run dev` / `npm run dev` | Start development server with hot reload |
| `bun run build` / `npm run build` | Build for production |
| `bun run preview` / `npm run preview` | Preview production build locally |
| `bun run check` / `npm run check` | Run Svelte type checking |
| `bun run check:watch` / `npm run check:watch` | Run type checking in watch mode |
| `bun run test:cli` / `npm run test:cli` | Verify latest CLI release selection and platform download links |
| `bun run test:privacy` / `npm run test:privacy` | Verify cookie consent and analytics gating |
| `bun run format` / `npm run format` | Format code with Prettier |
| `bun run lint` / `npm run lint` | Check code formatting |

## Project Structure

```
www/
├── src/
│   ├── lib/
│   │   ├── components/     # Reusable UI components
│   │   ├── config.ts       # Site configuration
│   │   └── utils.ts        # Utility functions
│   ├── routes/
│   │   ├── +page.svelte    # Homepage
│   │   ├── +layout.svelte  # Root layout
│   │   └── models/         # Models catalog page
│   ├── app.html            # HTML template
│   └── app.css             # Global styles
├── static/                 # Static assets (images, fonts)
├── package.json
├── svelte.config.js        # SvelteKit configuration
├── tailwind.config.ts      # TailwindCSS configuration
└── vite.config.ts          # Vite configuration
```

## Building for Production

```bash
# Create production build
bun run build        # Using Bun
# or
npm run build        # Using npm

# Test the production build locally
bun run preview      # Using Bun
# or
npm run preview      # Using npm
```

The built files will be in `.svelte-kit/output/`.

## Contributing

We welcome contributions to the Foundry Local website! Here's how to get started:

### Development Workflow

1. **Install dependencies**
   ```bash
   bun install        # Using Bun (recommended)
   # or
   npm install        # Using npm
   ```

2. **Make your changes**
   - Edit components in `src/lib/components/`
   - Update routes in `src/routes/`
   - Modify styles in `src/app.css` or component files

3. **Format your code**
   ```bash
   bun run format     # Auto-format with Prettier
   # or
   npm run format
   ```

4. **Run type checking**
   ```bash
   bun run check      # Verify TypeScript types
   # or
   npm run check
   ```

5. **Test locally**
   ```bash
   bun run dev        # Start dev server
   # or
   npm run dev
   ```

6. **Build and preview**
   ```bash
   bun run build && bun run preview
   # or
   npm run build && npm run preview
   ```

### Code Quality

- **Formatting**: Code is formatted with Prettier using the following settings:
  - Tabs for indentation
  - Single quotes
  - No trailing commas
  - 100 character line width
  - Svelte-specific formatting via `prettier-plugin-svelte`

- **Type Safety**: All code must pass TypeScript type checking. Run `bun run check` before committing.

- **Linting**: Code formatting is enforced via `bun run lint`. Run `bun run format` to auto-fix issues.

### Best Practices

- Always run `bun run format` before committing to ensure consistent code style
- Run `bun run check` to catch type errors early
- Test your changes in both development and production builds for significant features
- Preview production builds locally to verify optimizations work correctly

### Contributor License Agreement

This project requires contributors to sign a Contributor License Agreement (CLA). When you submit a pull request, a CLA bot will guide you through the process. For more details, see the [CONTRIBUTING.md](../CONTRIBUTING.md) file in the repository root.

## Deployment

This site is configured to deploy automatically to Vercel using `@sveltejs/adapter-auto`. Push to the main branch to trigger a deployment.

### Environment Variables

No environment variables are required for basic functionality. The site fetches model data from a public Azure Function endpoint.

### Cookie Consent

The root layout loads Microsoft's [WCP consent library](https://wcpstatic.microsoft.com/mscc/lib/v2/wcp-consent.js)
on every page. It displays the Microsoft consent banner for visitors without a saved choice and
persists preferences in the library-managed `MSCC` cookie. The **Manage Cookies** footer button on
the homepage, model catalog, and error pages reopens the preferences dialog.

Vercel Analytics is not injected until the library reports Analytics consent. Its `beforeSend`
callback checks current consent for each event, so rejecting Analytics later stops further events
without requiring a page reload. If the consent script fails to load or initialize, optional
analytics stay disabled and an accessible error message offers a retry.

After deployment, verify the banner in a fresh browser session, test Accept and Reject, and use
**Manage Cookies** to change a saved choice. Check that no Vercel Analytics script or events load
before consent or after rejection (an already-loaded script remains after withdrawal, but events
are blocked). Repeat on `/models` and request a new ComplianceWeb scan of the deployed site.

## About Foundry Local

Foundry Local enables you to run AI models locally on your device with:

- **On-Device Inference** - All processing stays on your hardware
- **Complete Privacy** - No data sent to external services
- **OpenAI-Compatible API** - Easy integration with existing tools
- **Hardware Acceleration** - Support for NPU, GPU, and CPU

For more information, visit the [official documentation](https://learn.microsoft.com/en-us/azure/ai-foundry/foundry-local/get-started).

## License

MIT License - Copyright (c) Microsoft Corporation

## Support

- **Documentation**: [Microsoft Learn](https://learn.microsoft.com/en-us/azure/ai-foundry/foundry-local/get-started)
- **Issues**: [GitHub Issues](https://github.com/microsoft/foundry-local/issues)
- **Community**: [GitHub Discussions](https://github.com/microsoft/foundry-local/discussions)
