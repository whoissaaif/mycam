package io.github.whoissaaif.mycam

import org.junit.Assert.assertTrue
import org.junit.Test
import org.w3c.dom.Element
import java.nio.file.Files
import java.nio.file.Path
import javax.xml.parsers.DocumentBuilderFactory

class BackupRulesTest {
    @Test
    fun fullBackupExcludesMycamSharedPrefs() {
        val backupRules = parseXml(findXml("backup_rules.xml"))
        assertTrue(hasExclude(backupRules.documentElement, "mycam.xml"))
    }

    @Test
    fun dataExtractionExcludesMycamSharedPrefsForCloudAndDeviceTransfer() {
        val extractionRules = parseXml(findXml("data_extraction_rules.xml")).documentElement
        assertTrue(hasExclude(findSection(extractionRules, "cloud-backup"), "mycam.xml"))
        assertTrue(hasExclude(findSection(extractionRules, "device-transfer"), "mycam.xml"))
    }

    private fun parseXml(path: Path) =
        DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(path.toFile())

    private fun findSection(root: Element, tagName: String): Element =
        root.getElementsByTagName(tagName).item(0) as Element

    private fun hasExclude(root: Element, prefFileName: String): Boolean {
        val excludes = root.getElementsByTagName("exclude")
        for (i in 0 until excludes.length) {
            val entry = excludes.item(i) as? Element ?: continue
            if (entry.getAttribute("domain") == "sharedpref" && entry.getAttribute("path") == prefFileName) {
                return true
            }
        }
        return false
    }

    private fun findXml(fileName: String): Path {
        var dir: Path? = Path.of("").toAbsolutePath()
        while (dir != null) {
            val modulePath = dir.resolve("src/main/res/xml/$fileName")
            if (Files.exists(modulePath)) return modulePath
            val projectPath = dir.resolve("app/src/main/res/xml/$fileName")
            if (Files.exists(projectPath)) return projectPath
            dir = dir.parent
        }
        throw IllegalStateException("Could not find $fileName")
    }
}
