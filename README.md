# ESP32-MeshWlan

ESP32 Evaluation for Mesh networks, Wlan Config sync 

Verbinde den ESP32 mit deinem WLAN (entweder automatisch oder über das WiFiManager Portal).

Öffne im Browser die IP des ESP32 (wird im Serial Monitor angezeigt).
Du siehst eine Liste aller gespeicherten Netzwerke.
**Löschen: **Ein Klick auf "Löschen" entfernt den Eintrag aus der networks.json. Er ist dann beim nächsten Neustart nicht mehr in der WiFiMulti-Suche.
**Reset: **Der Link löscht die gesamte Datei und startet den ESP neu – er wird also sofort wieder das WiFiManager-Portal öffnen.

**Interaktiver Scan**: 
Wenn du auf "Netzwerke suchen" klickst, scannt der ESP32 die Umgebung und zeigt eine Liste aller verfügbaren SSIDs mit ihrer Signalstärke an.

**Direktes Hinzufügen**: 
Neben jedem gefundenen Netzwerk ist ein Eingabefeld für das Passwort. Beim Klick auf "Speichern" wird das Netz in die networks.json geschrieben und sofort dem aktiven WiFiMulti-Pool hinzugefügt.

**Intelligentes Update**
: Wenn du ein Passwort für ein bereits bekanntes Netzwerk änderst, wird der bestehende Eintrag in der JSON-Datei aktualisiert statt ein Duplikat zu erstellen.

**Übersicht**: 
Du siehst jederzeit oben in der Tabelle, welche Netzwerke dein ESP32 bereits "kennt".


**Boot-Phase:**
Der ESP32 lädt alle JSON-Einträge.
WiFiMulti versucht eine Verbindung.

**Bedarfsfall AP:** 
Schlägt dies fehl, übernimmt WiFiManager und erstellt den AP "ESP32_SWARM_NET". In diesem Moment kannst du dich mit dem Handy verbinden und die Ersteinrichtung machen.

Im **Betrieb**: Der Webserver ist aus.
**Bedarfsfall Wartung**: Möchtest du im laufenden Betrieb ein Netz löschen oder die Liste sehen, drückst du kurz den Button. Der Server startet für 5 Minuten und schaltet sich dann wieder ab.

**Dynamik**:
Wenn du über den WiFiManager ein neues Netz hinzufügst, wird es automatisch in die JSON-Datei geschrieben und ist beim nächsten Mal Teil der schnellen WiFiMulti-Suche.



**Hochladen des Dateisystems**
Das Hochladen des Codes und das Hochladen des Dateisystems sind zwei getrennte Vorgänge. Wenn du nur den Code hochlädst, bleibt das Dateisystem unberührt.


Verbinde deinen ESP32 per USB.
Klicke in der PlatformIO-Seitenleiste auf das **Ameisen-Icon** (Project Tasks).
Navigiere zu Default (oder deinem Board-Namen) -> Platform.
Wähle dort **Upload Filesystem Image**.
PlatformIO packt nun den Inhalt deines data-Ordners in ein LittleFS-Image und schreibt es auf den reservierten Speicherbereich des ESP32.
