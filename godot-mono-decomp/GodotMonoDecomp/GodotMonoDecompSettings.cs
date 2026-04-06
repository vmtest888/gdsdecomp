

using ICSharpCode.Decompiler;
using ICSharpCode.Decompiler.CSharp;

namespace GodotMonoDecomp;

public class GodotMonoDecompSettings : DecompilerSettings
{
	/// <summary>
	/// Whether to write NuGet package references to the project file if dependency information is available.
	/// </summary>
	public bool WriteNuGetPackageReferences { get; set; } = true;

	/// <summary>
	/// Whether to check the hash of the NuGet package against the hash on nuget.org before writing project references.
	/// WARNING: This involves downloading the package from nuget.org and checking the hash of the downloaded package.
	/// </summary>
	public bool VerifyNuGetPackageIsFromNugetOrg { get; set; } = false;

	/// <summary>
	/// Whether to copy out-of-tree references (i.e. references that
	/// are not within the same directory structure as the project file) to the project file.
	/// </summary>
	public bool CopyOutOfTreeReferences { get; set; } = true;

	/// <summary>
	/// Whether to create additional projects for project references in main module.
	/// </summary>
	public bool CreateAdditionalProjectsForProjectReferences { get; set; } = true;

	/// <summary>
	/// Override the language version for the decompilation.
	/// </summary>
	public LanguageVersion? OverrideLanguageVersion { get; set; } = null;

	/// <summary>
	/// Godot version override for writing the SDK string in the project file.
	/// </summary>
	public Version? GodotVersionOverride { get; set; } = null;

	/// <summary>
	/// Whether to remove the body of the generated JsonSourceGeneration context classes.
	/// </summary>
	public bool RemoveGeneratedJsonContextBody { get; set; } = false;

	/// <summary>
	/// Whether to run LiftCollectionInitializers.
	/// If false, the legacy RemoveBogusBaseConstructorCalls transform is used instead.
	/// </summary>
	public bool EnableCollectionInitializerLifting { get; set; } = true;

	/// <summary>
	/// Emit ILInstruction annotations as comments for statement/expression nodes.
	/// Intended for debug verification of annotation propagation.
	/// </summary>
	public bool EmitILAnnotationComments { get; set; } = false;

	private void InitializeDefaultSettings()
	{
		UseNestedDirectoriesForNamespaces = true;
		// This avoids certain race conditions during static initialization when attempting to run the decompiled project.
		AlwaysMoveInitializer = true;
	}

	public GodotMonoDecompSettings() : base()
	{
		InitializeDefaultSettings();
	}

	public GodotMonoDecompSettings(LanguageVersion languageVersion) : base(languageVersion)
	{
		InitializeDefaultSettings();
	}


	public new GodotMonoDecompSettings Clone()
	{
		var settings = (GodotMonoDecompSettings)base.Clone();
		settings.WriteNuGetPackageReferences = WriteNuGetPackageReferences;
		settings.VerifyNuGetPackageIsFromNugetOrg = VerifyNuGetPackageIsFromNugetOrg;
		settings.CopyOutOfTreeReferences = CopyOutOfTreeReferences;
		settings.CreateAdditionalProjectsForProjectReferences = CreateAdditionalProjectsForProjectReferences;
		settings.OverrideLanguageVersion = OverrideLanguageVersion;
		settings.GodotVersionOverride = GodotVersionOverride;
		settings.RemoveGeneratedJsonContextBody = RemoveGeneratedJsonContextBody;
		settings.EnableCollectionInitializerLifting = EnableCollectionInitializerLifting;
		settings.EmitILAnnotationComments = EmitILAnnotationComments;
		return settings;
	}

}
