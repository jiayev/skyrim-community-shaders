unit AdvancedSkinPublishARMA;

var
  TargetRecord: IInterface;
  SelectionCount: Integer;

function Initialize: Integer;
begin
  Result := 0;
  SelectionCount := 0;
end;

function Process(e: IInterface): Integer;
begin
  Result := 0;
  Inc(SelectionCount);
  TargetRecord := e;
end;

function Finalize: Integer;
var
  OpenDialog: TOpenDialog;
  SaveDialog: TSaveDialog;
  Draft, MaterialPackage, Assignment: TJsonObject;
  PatchFile, Source, SourceTexture, TextureCopy, TargetCopy, Link: IInterface;
  Stream: TFileStream;
  Choice, Field, PluginName, DraftPath, Destination, Pending: string;
begin
  Result := 0;
  if (SelectionCount <> 1) or (Signature(TargetRecord) <> 'ARMA') then begin
    AddMessage('Select exactly one ARMA record. This publisher only supports its explicit NAM0/NAM1 skin texture assignment.');
    Exit;
  end;

  Choice := InputBox('Advanced Skin', 'Publish male or female skin material? Enter male or female.', 'female');
  if LowerCase(Choice) = 'male' then
    Field := 'NAM0'
  else if LowerCase(Choice) = 'female' then
    Field := 'NAM1'
  else
    Exit;

  Source := WinningOverride(TargetRecord);
  Link := ElementBySignature(Source, Field);
  if not Assigned(Link) then begin
    AddMessage('No explicit skin TXST in this field. Author a NIF material instead.');
    Exit;
  end;
  SourceTexture := LinksTo(Link);
  if not Assigned(SourceTexture) then begin
    AddMessage('Skin TXST is unresolved. No files were created.');
    Exit;
  end;
  if Signature(SourceTexture) <> 'TXST' then begin
    AddMessage('The selected field does not reference a TXST.');
    Exit;
  end;
  SourceTexture := WinningOverride(SourceTexture);

  OpenDialog := TOpenDialog.Create(nil);
  try
    OpenDialog.Title := 'Select a validated Advanced Skin material draft';
    OpenDialog.Filter := 'Skin material draft|*.skinmaterial.json|JSON|*.json';
    if not OpenDialog.Execute then Exit;
    DraftPath := OpenDialog.FileName;
  finally
    OpenDialog.Free;
  end;

  SaveDialog := TSaveDialog.Create(nil);
  try
    SaveDialog.Title := 'Choose a NEW plugin name and staging location';
    SaveDialog.Filter := 'Skyrim plugin|*.esp';
    SaveDialog.DefaultExt := 'esp';
    if not SaveDialog.Execute then Exit;
    PluginName := ExtractFileName(SaveDialog.FileName);
    Destination := ChangeFileExt(SaveDialog.FileName, '') + '-SkinPackage';
  finally
    SaveDialog.Free;
  end;
  if LowerCase(ExtractFileExt(PluginName)) <> '.esp' then
    raise Exception.Create('Use a new .esp filename.');
  if Assigned(FileByName(PluginName)) or FileExists(DataPath + PluginName) then
    raise Exception.Create('Plugin name is already in use. Choose a new name.');
  Pending := Destination + '.incomplete';
  if DirectoryExists(Destination) or DirectoryExists(Pending) then
    raise Exception.Create('Staging directory already exists. Choose a new location.');

  Draft := TJsonObject.Create;
  MaterialPackage := TJsonObject.Create;
  try
    Draft.LoadFromFile(DraftPath);
    if (Draft.I['schemaVersion'] <> 1) or not Draft.Contains('material') then
      raise Exception.Create('Expected a v1 material draft.');
    if (Draft.O['material'].O['parameters'].Count <> 20) or
       (Draft.O['material'].O['textures'].Count <> 2) then
      raise Exception.Create('Use the complete material exported by the Skin editor.');

    PatchFile := AddNewFileName(PluginName, False);
    if not Assigned(PatchFile) then Exit;
    AddRequiredElementMasters(Source, PatchFile, False);
    AddRequiredElementMasters(SourceTexture, PatchFile, False);
    TextureCopy := wbCopyElementToFile(SourceTexture, PatchFile, True, True);
    TargetCopy := wbCopyElementToFile(Source, PatchFile, False, True);
    if not Assigned(TextureCopy) or not Assigned(TargetCopy) then
      raise Exception.Create('Could not copy the complete records. Discard the new unsaved plugin.');
    SetElementEditValues(TargetCopy, Field, IntToHex(GetLoadOrderFormID(TextureCopy), 8));
    Link := LinksTo(ElementBySignature(TargetCopy, Field));
    if not Assigned(Link) then
      raise Exception.Create('Failed to link the new texture set. Discard the new unsaved plugin.');
    if GetLoadOrderFormID(Link) <> GetLoadOrderFormID(TextureCopy) then
      raise Exception.Create('Texture assignment verification failed. Discard the new unsaved plugin.');

    MaterialPackage.I['schemaVersion'] := 1;
    MaterialPackage.S['ownerPlugin'] := PluginName;
    Assignment := MaterialPackage.A['recordMaterials'].AddObject;
    Assignment.O['txst'].S['plugin'] := PluginName;
    Assignment.O['txst'].S['localFormId'] := IntToHex(FixedFormID(TextureCopy) and $00FFFFFF, 8);
    Assignment.O['material'].Assign(Draft.O['material']);
    MaterialPackage.A['raceAdjustments'].Clear;
    MaterialPackage.A['npcAdjustments'].Clear;

    ForceDirectories(Pending + '\Shaders\Skin\Materials');
    MaterialPackage.SaveToFile(Pending + '\Shaders\Skin\Materials\' + PluginName + '.skin.json', False, TEncoding.UTF8, True);
    Stream := TFileStream.Create(Pending + '\' + PluginName, fmCreate);
    try
      FileWriteToStream(PatchFile, Stream, 1);
    finally
      Stream.Free;
    end;
    if not RenameFile(Pending, Destination) then
      raise Exception.Create('Could not finalize staging directory; inspect the .incomplete directory.');
    AddMessage('Published plugin and material package: ' + Destination);
    AddMessage('The full TXST was copied. Only ' + Name(Source) + ' / ' + Field + ' was reassigned.');
    AddMessage('Validate the JSON with skin_materials.py, review the new plugin, then install the staging directory as a mod.');
    AddMessage('Do not compact FormIDs after publication without regenerating the material package.');
  finally
    MaterialPackage.Free;
    Draft.Free;
  end;
end;

end.
