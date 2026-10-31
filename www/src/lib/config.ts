import type { NavItem, SocialLink } from '$lib/types/nav';
import {
	Boxes,
	Paintbrush,
	Workflow,
	Zap,
	Rocket,
	Box,
	Code,
	Cpu,
	Bot,
	BookOpen,
	Wifi,
	Shield
} from 'lucide-svelte';
import type { Feature, PromoConfig, SiteConfig } from './types/config';

export const siteConfig: SiteConfig = {
	title: 'Foundry Local',
	description:
		'Use native SDKs to download, cache, load, and call optimized local models on-device.',
	github: 'https://github.com/microsoft/foundry-local',
	npm: '',
	quickLinks: [
		{
			title: 'Getting Started',
			href: 'https://learn.microsoft.com/en-us/azure/foundry-local/get-started'
		},
		{ title: 'GitHub', href: 'https://github.com/microsoft/foundry-local' }
	],
	logo: '/logos/foundry-local-logo-color.svg',
	logoDark: '/logos/foundry-local-logo-fill.svg',
	logoMark: '/logos/foundry-local-logo-stroke.svg',
	favicon: '/favicon.png'
};

export let navItems: NavItem[] = [
	{
		title: 'Models',
		href: '/models',
		icon: Box
	},
	{
		title: 'Docs',
		href: 'https://learn.microsoft.com/en-us/azure/foundry-local/get-started',
		icon: BookOpen
	}
];

export let socialLinks: SocialLink[] = [
	{
		title: 'GitHub',
		href: 'https://github.com/microsoft/foundry-local',
		icon: 'github'
	}
];

export const features: Feature[] = [
	{
		icon: Rocket,
		title: 'SDK-First App Integration',
		description: 'Initialize, download, load, and call models directly from your app process',
		size: 'large'
	},
	{
		icon: Cpu,
		title: 'Hardware Optimized',
		description: 'Automatic execution provider management for NPU, GPU, and CPU acceleration',
		size: 'large'
	},
	{
		icon: Wifi,
		title: 'Offline by Design',
		description: '~20 MB runtime and cached models keep apps running without a network',
		size: 'medium'
	},
	{
		icon: Code,
		title: 'Native SDKs',
		description: 'Build in C, C++, C#, JavaScript, Python, or Rust on the same native runtime',
		size: 'medium'
	},
	{
		icon: Bot,
		title: 'Native First, REST When Needed',
		description: 'Use SDK clients in-process, or start the optional OpenAI-compatible server',
		size: 'medium'
	},
	{
		icon: Shield,
		title: 'Data Privacy',
		description: 'Prompts, audio, and responses stay on the user device',
		size: 'medium'
	}
];

export let promoConfig: PromoConfig = {
	title: 'Need to scale to the cloud?',
	description:
		'Azure AI Foundry provides enterprise-scale AI infrastructure when your project outgrows local deployment.',
	ctaText: 'Learn about Azure AI Foundry',
	ctaLink: 'https://azure.microsoft.com/en-us/products/ai-studio',
	lightImage: '/images/cloud-scale-light.jpg',
	darkImage: '/images/cloud-scale-dark.jpg'
};
