plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release builds take their version from `-PappVersion=1.2.3` (the CI passes the git tag).
// versionCode must keep increasing for updates to install: 1.2.3 -> 10203.
val appVersion = (findProperty("appVersion") as String?) ?: "0.1"
val appVersionCode = appVersion.split(".").map { it.toIntOrNull() ?: 0 }.let { p ->
    p.getOrElse(0) { 0 } * 10000 + p.getOrElse(1) { 0 } * 100 + p.getOrElse(2) { 0 }
}.coerceAtLeast(1)

// Release signing, from environment variables so no secret lives in the repo.
val releaseKeystore = System.getenv("THLOGGER_KEYSTORE")?.let { file(it) }?.takeIf { it.exists() }

android {
    namespace = "io.github.vattern.thlogger"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.github.vattern.thlogger"
        minSdk = 31          // Android 12: BLUETOOTH_SCAN/CONNECT without location permission
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersion
    }

    signingConfigs {
        if (releaseKeystore != null) create("release") {
            storeFile = releaseKeystore
            storePassword = System.getenv("THLOGGER_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("THLOGGER_KEY_ALIAS")
            keyPassword = System.getenv("THLOGGER_KEY_PASSWORD")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Without a keystore the release APK is left unsigned (and won't install).
            signingConfig = signingConfigs.findByName("release")
        }
    }
    buildFeatures {
        compose = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.03.00"))
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.core:core-ktx:1.18.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.10.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.10.0")
    implementation("androidx.work:work-runtime-ktx:2.11.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
}
