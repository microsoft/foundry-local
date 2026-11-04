# Copyright (c) Microsoft Corporation. Licensed under the MIT License.
"""Offline fault tests: python test-transfer-retry.py --maven-home <bootstrapped Maven> --java-home <JDK>."""

import argparse
import base64
import collections
import hashlib
import http.server
import io
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import unittest
import zipfile


SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parent.parent
PARENT = b"""<project><modelVersion>4.0.0</modelVersion><groupId>ci.fixture</groupId>
<artifactId>parent</artifactId><version>1</version><packaging>pom</packaging></project>"""
PARENT_PATH = "/ci/fixture/parent/1/parent-1.pom"
LATE_JAR = "/ci/fixture/runtime/1/runtime-1.jar"
RETRY_MESSAGE = "Retrying interrupted Maven download"


class TransferRetryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = ROOT / "sdk_v2" / "java" / "build" / "maven-transfer-retry-tests" / os.name
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.scratch = tempfile.TemporaryDirectory(dir=cls.build)
        cls.addClassCleanup(cls.scratch.cleanup)
        work = Path(cls.scratch.name)
        cls.launcher = OPTIONS.maven_home / ("bin/mvn.cmd" if os.name == "nt" else "bin/mvn")
        if os.name != "nt":
            cls.launcher.chmod(cls.launcher.stat().st_mode | 0o100)
        extension = OPTIONS.maven_home / "lib/ext/maven-transfer-retry.jar"
        if not extension.is_file():
            raise RuntimeError("Run the shared Maven bootstrap before testing; retry extension is missing")
        cls.environment = dict(os.environ, JAVA_HOME=str(OPTIONS.java_home))
        bootstrap = (ROOT / ".pipelines/templates/steps-bootstrap-maven.yml").read_text()
        cls.environment["MAVEN_ARGS"] = " ".join(re.findall(r"^\s*'(-[^']+)'", bootstrap, re.MULTILINE))
        java = OPTIONS.java_home / "bin" / ("java.exe" if os.name == "nt" else "java")
        javac = OPTIONS.java_home / "bin" / ("javac.exe" if os.name == "nt" else "javac")
        mojo = work / "MarkerMojo.java"
        mojo.write_text("""
package ci.fixture;
public final class MarkerMojo extends org.apache.maven.plugin.AbstractMojo {
    private org.eclipse.aether.RepositorySystem repositorySystem;
    private org.apache.maven.execution.MavenSession session;
    private String stage;

    public void execute() throws org.apache.maven.plugin.MojoExecutionException,
            org.apache.maven.plugin.MojoFailureException {
        if ("after".equals(stage)) {
            try {
                repositorySystem.resolveArtifact(session.getRepositorySession(),
                    new org.eclipse.aether.resolution.ArtifactRequest(
                        new org.eclipse.aether.artifact.DefaultArtifact("ci.fixture:runtime:jar:1"),
                        java.util.Collections.singletonList(new org.eclipse.aether.repository.RemoteRepository.Builder(
                            "fixture", "default", System.getProperty("ci.url")).build()), null));
            } catch (org.eclipse.aether.resolution.ArtifactResolutionException failure) {
                throw new org.apache.maven.plugin.MojoExecutionException("Runtime download failed", failure);
            }
        }
        try {
            java.nio.file.Files.write(java.nio.file.Paths.get(System.getProperty("ci.marker")),
                java.util.Collections.singletonList("ran"), java.nio.file.StandardOpenOption.CREATE,
                java.nio.file.StandardOpenOption.APPEND);
        } catch (java.io.IOException failure) {
            throw new org.apache.maven.plugin.MojoExecutionException("Marker write failed", failure);
        }
        if (Boolean.getBoolean("ci.fail")) {
            throw new org.apache.maven.plugin.MojoFailureException("Intentional test failure");
        }
    }
}
""")
        classpath = os.pathsep.join((str(OPTIONS.maven_home / "lib/*"), str(extension)))
        compile_result = subprocess.run(
            [str(javac), "--release", "8", "-cp", classpath, "-d", str(work),
             str(mojo), str(SOURCE / "RetryingHttpTransporterFactoryTest.java")],
            capture_output=True, text=True,
        )
        if compile_result.returncode:
            raise AssertionError(compile_result.stdout + compile_result.stderr)
        policy = subprocess.run(
            [str(java), "-cp", os.pathsep.join((str(work), classpath)),
             "com.microsoft.foundry.ci.RetryingHttpTransporterFactoryTest"],
            capture_output=True, text=True, timeout=60,
        )
        (cls.build / "policy.log").write_text(policy.stdout + policy.stderr)
        if policy.returncode:
            raise AssertionError(policy.stdout + policy.stderr)
        cls.artifacts = {PARENT_PATH: PARENT}
        for name in ("before", "after"):
            base = f"/ci/fixture/{name}/1/{name}-1"
            cls.artifacts[base + ".pom"] = (
                "<project><modelVersion>4.0.0</modelVersion><groupId>ci.fixture</groupId>"
                f"<artifactId>{name}</artifactId><version>1</version><packaging>maven-plugin</packaging></project>"
            ).encode()
            descriptor = f"""<plugin><name>Fixture</name><groupId>ci.fixture</groupId>
<artifactId>{name}</artifactId><version>1</version><goalPrefix>{name}</goalPrefix><mojos><mojo>
<goal>record</goal><implementation>ci.fixture.MarkerMojo</implementation><language>java</language>
<instantiationStrategy>per-lookup</instantiationStrategy><executionStrategy>once-per-session</executionStrategy>
<threadSafe>true</threadSafe><parameters>
<parameter><name>session</name><type>org.apache.maven.execution.MavenSession</type><readonly>true</readonly></parameter>
<parameter><name>stage</name><type>java.lang.String</type></parameter></parameters>
<configuration><session implementation="org.apache.maven.execution.MavenSession" default-value="${{session}}"/>
<stage implementation="java.lang.String">{name}</stage></configuration><requirements>
<requirement><role>org.eclipse.aether.RepositorySystem</role><field-name>repositorySystem</field-name></requirement>
</requirements></mojo></mojos></plugin>"""
            jar = io.BytesIO()
            with zipfile.ZipFile(jar, "w") as archive:
                archive.writestr("META-INF/maven/plugin.xml", descriptor)
                archive.writestr("ci/fixture/MarkerMojo.class", (work / "ci/fixture/MarkerMojo.class").read_bytes())
            cls.artifacts[base + ".jar"] = jar.getvalue()
        cls.artifacts[LATE_JAR] = cls.artifacts["/ci/fixture/after/1/after-1.jar"]

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(dir=self.build)
        self.addCleanup(scratch.cleanup)
        self.work = Path(scratch.name)
        self.requests = collections.Counter()
        self.ranges = []
        self.fault_path = PARENT_PATH
        self.failures = 0
        self.status = 200
        self.require_authentication = False
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                fixture.requests[self.path] += 1
                if fixture.require_authentication:
                    expected = "Basic " + base64.b64encode(b"fixture:fixture").decode()
                    if self.headers.get("Authorization") != expected:
                        self.send_response(401)
                        self.send_header("WWW-Authenticate", 'Basic realm="fixture"')
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                data = fixture.artifacts.get(self.path)
                if self.path.endswith(".sha1"):
                    artifact = fixture.artifacts.get(self.path[:-5])
                    if artifact is not None:
                        data = hashlib.sha1(artifact).hexdigest().encode()
                if data is None:
                    self.send_error(404)
                    return
                if self.path == fixture.fault_path:
                    fixture.ranges.append(self.headers.get("Range"))
                    if fixture.failures:
                        fixture.failures -= 1
                        self.send_response(fixture.status)
                        self.send_header("Content-Length", str(len(data) if fixture.status == 200 else 0))
                        self.end_headers()
                        if fixture.status == 200:
                            self.wfile.write(data[:16])
                            self.wfile.flush()
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.close_connection = True
                        return
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.addCleanup(self.server.server_close)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.server.shutdown)
        self.settings = self.work / "settings.xml"
        self.settings.write_text(
            "<settings><mirrors><mirror><id>fixture</id><mirrorOf>*</mirrorOf>"
            f"<url>http://127.0.0.1:{self.server.server_port}</url></mirror></mirrors></settings>"
        )
        self.pom = self.work / "pom.xml"
        self.pom.write_text(
            "<project><modelVersion>4.0.0</modelVersion><parent><groupId>ci.fixture</groupId>"
            "<artifactId>parent</artifactId><version>1</version><relativePath/></parent>"
            "<artifactId>child</artifactId><packaging>pom</packaging></project>"
        )

    def maven(self, *arguments, repository="repository", goal="validate"):
        command = [str(self.launcher), "--batch-mode", "--no-transfer-progress",
                   "--settings", str(self.settings), f"-Dmaven.repo.local={self.work / repository}",
                   "-f", str(self.pom), "-U", f"-Dci.url=http://127.0.0.1:{self.server.server_port}",
                   *arguments, goal]
        if os.name == "nt":
            command = [os.environ["COMSPEC"], "/d", "/c", *command]
        result = subprocess.run(command, env=self.environment, capture_output=True, text=True, timeout=90)
        with (self.build / f"{self.id().split('.')[-1]}.log").open("a") as log:
            log.write(result.stdout + result.stderr)
        return result

    def assert_artifact(self, path, repository="repository"):
        self.assertEqual((self.work / repository / path.lstrip("/")).read_bytes(), self.artifacts[path])

    def test_control_without_decorator_fails(self):
        self.failures = 1
        result = self.maven("-Daether.priority.RetryingHttpTransporterFactory=-1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.requests[PARENT_PATH], 1)
        self.assertNotIn(RETRY_MESSAGE, result.stdout)

    def test_truncated_body_recovers_in_both_repositories(self):
        for repository in ("repository", "consumer-repository"):
            self.failures = 1
            result = self.maven(repository=repository)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout.count(RETRY_MESSAGE), 1)
            self.assert_artifact(PARENT_PATH, repository)
        self.assertEqual(self.requests[PARENT_PATH], 4)

    def test_partial_body_is_restarted_without_corruption(self):
        self.failures = 1
        result = self.maven()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.requests[PARENT_PATH], 2)
        # Resolver 1.9.22's basic connector explicitly disables resumption on GetTask.
        self.assertEqual(self.ranges, [None, None])
        self.assert_artifact(PARENT_PATH)

    def test_exhaustion_remains_fatal_then_can_recover(self):
        self.failures = 100
        result = self.maven()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.requests[PARENT_PATH], 3)
        self.assertEqual(result.stdout.count(RETRY_MESSAGE), 2)
        self.assertFalse((self.work / "repository" / PARENT_PATH.lstrip("/")).exists())
        self.failures = 0
        result = self.maven()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_artifact(PARENT_PATH)

    def test_permanent_http_errors_are_not_retried(self):
        for status in (401, 403, 404):
            with self.subTest(status=status):
                self.status, self.failures = status, 100
                before = self.requests[PARENT_PATH]
                result = self.maven(repository=str(status))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(self.requests[PARENT_PATH] - before, 1)
                self.assertNotIn(RETRY_MESSAGE, result.stdout)

    def test_native_status_retry_still_works(self):
        self.status, self.failures = 502, 1
        result = self.maven()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.requests[PARENT_PATH], 2)
        self.assertNotIn(RETRY_MESSAGE, result.stdout)

    def test_authenticated_download_recovers(self):
        self.require_authentication = True
        self.failures = 1
        self.settings.write_text(self.settings.read_text().replace(
            "<settings>", "<settings><servers><server><id>fixture</id><username>fixture</username>"
            "<password>fixture</password></server></servers>",
        ))
        result = self.maven()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.count(RETRY_MESSAGE), 1)
        self.assert_artifact(PARENT_PATH)

    def test_invalid_project_does_not_retry(self):
        self.pom.write_text("<project>")
        result = self.maven()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.requests)
        self.assertNotIn(RETRY_MESSAGE, result.stdout)

    def lifecycle(self):
        plugins = "".join(
            "<plugin><groupId>ci.fixture</groupId>"
            f"<artifactId>{name}</artifactId><version>1</version><executions><execution>"
            f"<phase>{phase}</phase><goals><goal>record</goal></goals></execution></executions></plugin>"
            for name, phase in (("before", "test"), ("after", "verify"))
        )
        self.pom.write_text(
            "<project><modelVersion>4.0.0</modelVersion><groupId>ci.fixture</groupId>"
            "<artifactId>lifecycle</artifactId><version>1</version><packaging>pom</packaging>"
            f"<build><plugins>{plugins}</plugins></build></project>"
        )
        self.fault_path = LATE_JAR
        self.marker = self.work / "executions.txt"

    def test_late_plugin_download_does_not_replay_test_goal(self):
        self.lifecycle()
        self.failures = 1
        result = self.maven(f"-Dci.marker={self.marker}", goal="verify")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.marker.read_text().splitlines(), ["ran", "ran"])
        self.assertEqual(self.requests[LATE_JAR], 2)
        self.assert_artifact(LATE_JAR)

    def test_persistent_late_failure_preserves_cache(self):
        self.lifecycle()
        self.failures = 100
        result = self.maven(f"-Dci.marker={self.marker}", goal="verify")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.marker.read_text().splitlines(), ["ran"])
        self.assertEqual(self.requests[LATE_JAR], 3)
        early = "/ci/fixture/before/1/before-1.jar"
        self.assert_artifact(early)
        before = self.requests[early]
        self.failures = 0
        result = self.maven(f"-Dci.marker={self.marker}", goal="verify")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.requests[early], before)
        self.assert_artifact(LATE_JAR)

    def test_real_test_failure_is_not_retried(self):
        self.lifecycle()
        result = self.maven(f"-Dci.marker={self.marker}", "-Dci.fail=true", goal="verify")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.marker.read_text().splitlines(), ["ran"])
        self.assertEqual(self.requests[LATE_JAR], 0)
        self.assertNotIn(RETRY_MESSAGE, result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maven-home", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--java-home", type=lambda value: Path(value).resolve(), required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
