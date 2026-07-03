import type { GroupedFoundryModel } from './types';

export type StarterLanguage = 'python' | 'javascript' | 'csharp' | 'rust';
export type StarterKind = 'chat' | 'audio' | 'embedding';

export type StarterLanguageOption = {
	id: StarterLanguage;
	label: string;
	shortLabel: string;
	fileName: string;
	installCommand: string;
};

export type StarterSnippet = StarterLanguageOption & {
	code: string;
	kind: StarterKind;
};

export const STARTER_LANGUAGES: StarterLanguageOption[] = [
	{
		id: 'python',
		label: 'Python',
		shortLabel: 'Py',
		fileName: 'app.py',
		installCommand: 'pip install foundry-local-sdk'
	},
	{
		id: 'javascript',
		label: 'JavaScript',
		shortLabel: 'JS',
		fileName: 'app.js',
		installCommand: 'npm install foundry-local-sdk'
	},
	{
		id: 'csharp',
		label: 'C#',
		shortLabel: 'C#',
		fileName: 'Program.cs',
		installCommand: 'dotnet add package Microsoft.AI.Foundry.Local'
	},
	{
		id: 'rust',
		label: 'Rust',
		shortLabel: 'Rs',
		fileName: 'main.rs',
		installCommand: 'cargo add foundry-local-sdk'
	}
];

const DEVICE_SUFFIX_PATTERN = /-(generic|cuda|qnn|openvino|vitis)-(cpu|gpu|npu)$|-(cpu|gpu|npu)$/i;

export function getGenericModelName(model: GroupedFoundryModel): string {
	const baseName = model.alias || model.variants[0]?.name || model.displayName;
	const withoutVersion = baseName.split(':')[0];
	return withoutVersion.replace(DEVICE_SUFFIX_PATTERN, '');
}

export function getModelStarterKind(model: GroupedFoundryModel): StarterKind {
	const searchable = [
		model.taskType,
		model.alias,
		model.displayName,
		...(model.tags ?? [])
	]
		.filter(Boolean)
		.join(' ')
		.toLowerCase();

	if (
		searchable.includes('automatic-speech-recognition') ||
		searchable.includes('speech-to-text') ||
		searchable.includes('whisper')
	) {
		return 'audio';
	}

	if (searchable.includes('embedding') || searchable.includes('embeddings')) {
		return 'embedding';
	}

	return 'chat';
}

export function getStarterKindLabel(kind: StarterKind): string {
	switch (kind) {
		case 'audio':
			return 'Audio transcription';
		case 'embedding':
			return 'Embeddings';
		default:
			return 'Chat completion';
	}
}

export function getStarterHint(kind: StarterKind): string {
	switch (kind) {
		case 'audio':
			return 'Pass an audio file path as the first argument.';
		case 'embedding':
			return 'Generates one text embedding and prints its dimensions.';
		default:
			return 'Sends one prompt to the local chat client.';
	}
}

export function getStarterSnippet(
	model: GroupedFoundryModel,
	language: StarterLanguage
): StarterSnippet {
	const languageOption = STARTER_LANGUAGES.find((item) => item.id === language) ?? STARTER_LANGUAGES[0];
	const modelId = getGenericModelName(model);
	const kind = getModelStarterKind(model);

	return {
		...languageOption,
		kind,
		code: createStarterCode(languageOption.id, kind, modelId)
	};
}

function createStarterCode(language: StarterLanguage, kind: StarterKind, modelId: string): string {
	switch (language) {
		case 'javascript':
			return createJavaScriptStarter(kind, modelId);
		case 'csharp':
			return createCSharpStarter(kind, modelId);
		case 'rust':
			return createRustStarter(kind, modelId);
		default:
			return createPythonStarter(kind, modelId);
	}
}

function createPythonStarter(kind: StarterKind, modelId: string): string {
	const shared = [
		'from foundry_local_sdk import Configuration, FoundryLocalManager',
		'',
		'FoundryLocalManager.initialize(Configuration(app_name="model_starter"))',
		'manager = FoundryLocalManager.instance',
		`model = manager.catalog.get_model("${modelId}")`,
		'model.download()',
		'model.load()',
		''
	];

	if (kind === 'audio') {
		return [
			'import sys',
			...shared,
			'audio_file = sys.argv[1] if len(sys.argv) > 1 else "Recording.mp3"',
			'client = model.get_audio_client()',
			'result = client.transcribe(audio_file)',
			'print(result.text)',
			'',
			'model.unload()'
		].join('\n');
	}

	if (kind === 'embedding') {
		return [
			...shared,
			'client = model.get_embedding_client()',
			'response = client.generate_embedding("The quick brown fox jumps over the lazy dog")',
			'print(f"Dimensions: {len(response.data[0].embedding)}")',
			'',
			'model.unload()'
		].join('\n');
	}

	return [
		...shared,
		'client = model.get_chat_client()',
		'response = client.complete_chat([',
		'    {"role": "user", "content": "What can you help me build?"}',
		'])',
		'print(response.choices[0].message.content)',
		'',
		'model.unload()'
	].join('\n');
}

function createJavaScriptStarter(kind: StarterKind, modelId: string): string {
	const shared = [
		"import { FoundryLocalManager } from 'foundry-local-sdk';",
		'',
		"const manager = FoundryLocalManager.create({ appName: 'model_starter' });",
		`const model = await manager.catalog.getModel('${modelId}');`,
		'await model.download();',
		'await model.load();',
		''
	];

	if (kind === 'audio') {
		return [
			...shared,
			'const audioFile = process.argv[2] || "Recording.mp3";',
			'const client = model.createAudioClient();',
			'const result = await client.transcribe(audioFile);',
			'console.log(result.text);',
			'',
			'await model.unload();'
		].join('\n');
	}

	if (kind === 'embedding') {
		return [
			...shared,
			'const client = model.createEmbeddingClient();',
			"const response = await client.generateEmbedding('The quick brown fox jumps over the lazy dog');",
			'console.log(`Dimensions: ${response.data[0].embedding.length}`);',
			'',
			'await model.unload();'
		].join('\n');
	}

	return [
		...shared,
		'const client = model.createChatClient();',
		'const response = await client.completeChat([',
		"  { role: 'user', content: 'What can you help me build?' }",
		']);',
		'console.log(response.choices[0]?.message?.content);',
		'',
		'await model.unload();'
	].join('\n');
}

function createCSharpStarter(kind: StarterKind, modelId: string): string {
	const imports =
		kind === 'chat'
			? [
					'using Microsoft.AI.Foundry.Local;',
					'using Microsoft.Extensions.Logging.Abstractions;',
					'using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;'
				]
			: ['using Microsoft.AI.Foundry.Local;', 'using Microsoft.Extensions.Logging.Abstractions;'];

	const shared = [
		...imports,
		'',
		'await FoundryLocalManager.CreateAsync(',
		'    new Configuration { AppName = "model_starter" },',
		'    NullLogger.Instance);',
		'',
		'var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();',
		`var model = await catalog.GetModelAsync("${modelId}")`,
		'    ?? throw new Exception("Model not found.");',
		'',
		'await model.DownloadAsync();',
		'await model.LoadAsync();',
		''
	];

	if (kind === 'audio') {
		return [
			...shared,
			'var audioFile = args.Length > 0 ? args[0] : "Recording.mp3";',
			'var client = await model.GetAudioClientAsync();',
			'var stream = client.TranscribeAudioStreamingAsync(audioFile, CancellationToken.None);',
			'await foreach (var chunk in stream)',
			'{',
			'    Console.Write(chunk.Text);',
			'}',
			'Console.WriteLine();',
			'',
			'await model.UnloadAsync();'
		].join('\n');
	}

	if (kind === 'embedding') {
		return [
			...shared,
			'var client = await model.GetEmbeddingClientAsync();',
			'var response = await client.GenerateEmbeddingAsync(',
			'    "The quick brown fox jumps over the lazy dog");',
			'Console.WriteLine($"Dimensions: {response.Data[0].Embedding.Count}");',
			'',
			'await model.UnloadAsync();'
		].join('\n');
	}

	return [
		...shared,
		'var client = await model.GetChatClientAsync();',
		'var response = await client.CompleteChatAsync(new[]',
		'{',
		'    new ChatMessage { Role = "user", Content = "What can you help me build?" }',
		'});',
		'Console.WriteLine(response.Choices![0].Message.Content);',
		'',
		'await model.UnloadAsync();'
	].join('\n');
}

function createRustStarter(kind: StarterKind, modelId: string): string {
	if (kind === 'audio') {
		return [
			'use std::env;',
			'use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};',
			'',
			'#[tokio::main]',
			'async fn main() -> Result<(), Box<dyn std::error::Error>> {',
			'    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("model_starter"))?;',
			`    let model = manager.catalog().get_model("${modelId}").await?;`,
			'    model.download(None::<fn(f64)>).await?;',
			'    model.load().await?;',
			'',
			'    let audio_path = env::args().nth(1).unwrap_or_else(|| "Recording.mp3".to_string());',
			'    let client = model.create_audio_client();',
			'    let result = client.transcribe(&audio_path).await?;',
			'    println!("{}", result.text);',
			'',
			'    model.unload().await?;',
			'    Ok(())',
			'}'
		].join('\n');
	}

	if (kind === 'embedding') {
		return [
			'use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};',
			'',
			'#[tokio::main]',
			'async fn main() -> Result<(), Box<dyn std::error::Error>> {',
			'    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("model_starter"))?;',
			`    let model = manager.catalog().get_model("${modelId}").await?;`,
			'    model.download(None::<fn(f64)>).await?;',
			'    model.load().await?;',
			'',
			'    let client = model.create_embedding_client();',
			'    let response = client',
			'        .generate_embedding("The quick brown fox jumps over the lazy dog")',
			'        .await?;',
			'    println!("Dimensions: {}", response.data[0].embedding.len());',
			'',
			'    model.unload().await?;',
			'    Ok(())',
			'}'
		].join('\n');
	}

	return [
		'use foundry_local_sdk::{',
		'    ChatCompletionRequestMessage, ChatCompletionRequestUserMessage, FoundryLocalConfig,',
		'    FoundryLocalManager,',
		'};',
		'',
		'#[tokio::main]',
		'async fn main() -> Result<(), Box<dyn std::error::Error>> {',
		'    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("model_starter"))?;',
		`    let model = manager.catalog().get_model("${modelId}").await?;`,
		'    model.download(None::<fn(f64)>).await?;',
		'    model.load().await?;',
		'',
		'    let client = model.create_chat_client();',
		'    let messages: Vec<ChatCompletionRequestMessage> = vec![',
		'        ChatCompletionRequestUserMessage::from("What can you help me build?").into(),',
		'    ];',
		'    let response = client.complete_chat(&messages, None).await?;',
		'    if let Some(choice) = response.choices.first() {',
		'        if let Some(content) = &choice.message.content {',
		'            println!("{content}");',
		'        }',
		'    }',
		'',
		'    model.unload().await?;',
		'    Ok(())',
		'}'
	].join('\n');
}
