// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QtCore/QtGlobal>

#ifdef Q_OS_WIN
#	include "win.h"
#endif

#include "Meta.h"
#include "SelfSignedCertificate.h"
#include "Server.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#ifdef Q_OS_WIN
#	include <winsock2.h>
#endif

bool Server::isKeyForCert(const QSslKey &key, const QSslCertificate &cert) {
	if (key.isNull() || cert.isNull() || (key.type() != QSsl::PrivateKey))
		return false;

	QByteArray qbaKey  = key.toDer();
	QByteArray qbaCert = cert.toDer();

	X509 *x509     = nullptr;
	EVP_PKEY *pkey = nullptr;
	BIO *mem       = nullptr;

	mem  = BIO_new_mem_buf(qbaKey.data(), static_cast< int >(qbaKey.size()));
	pkey = d2i_PrivateKey_bio(mem, nullptr);
	BIO_free(mem);

	mem  = BIO_new_mem_buf(qbaCert.data(), static_cast< int >(qbaCert.size()));
	x509 = d2i_X509_bio(mem, nullptr);
	BIO_free(mem);
	mem = nullptr;

	if (x509 && pkey && X509_check_private_key(x509, pkey)) {
		EVP_PKEY_free(pkey);
		X509_free(x509);
		return true;
	}

	if (pkey)
		EVP_PKEY_free(pkey);
	if (x509)
		X509_free(x509);
	return false;
}

QSslKey Server::privateKeyFromPEM(const QByteArray &buf, const QByteArray &pass) {
	QSslKey key;
	key = QSslKey(buf, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, pass);
	if (key.isNull())
		key = QSslKey(buf, QSsl::Dsa, QSsl::Pem, QSsl::PrivateKey, pass);
	if (key.isNull())
		key = QSslKey(buf, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey, pass);
	return key;
}

void Server::initializeCert() {
	QByteArray crt, key, pass, dhparams;

	// Clear all existing SSL settings
	// for this server.
	qscCert.clear();
	qlIntermediates.clear();
	qskKey.clear();
#if defined(USE_QSSLDIFFIEHELLMANPARAMETERS)
	qsdhpDHParams = QSslDiffieHellmanParameters();
#endif

	m_dbWrapper.getConfigurationTo(iServerNum, "certificate", crt);
	m_dbWrapper.getConfigurationTo(iServerNum, "key", key);
	m_dbWrapper.getConfigurationTo(iServerNum, "passphrase", pass);
	m_dbWrapper.getConfigurationTo(iServerNum, "sslDHParams", dhparams);

	QList< QSslCertificate > ql;

	// Attempt to load the private key.
	if (!key.isEmpty()) {
		qskKey = Server::privateKeyFromPEM(key, pass);
	}

	// If we still can't load the key, try loading any keys from the certificate
	if (qskKey.isNull() && !crt.isEmpty()) {
		qskKey = Server::privateKeyFromPEM(crt);
	}

	// If have a key, walk the list of certs, find the one for our key,
	// remove any certs for our key from the list, what's left is part of
	// the CA certificate chain.
	if (!qskKey.isNull()) {
		ql << QSslCertificate::fromData(crt);
		ql << QSslCertificate::fromData(key);
		for (int i = 0; i < ql.size(); ++i) {
			const QSslCertificate &c = ql.at(i);
			if (isKeyForCert(qskKey, c)) {
				qscCert = c;
				ql.removeAt(i);
			}
		}
		qlIntermediates = ql;
	}

#if defined(USE_QSSLDIFFIEHELLMANPARAMETERS)
	if (!dhparams.isEmpty()) {
		QSslDiffieHellmanParameters qdhp = QSslDiffieHellmanParameters::fromEncoded(dhparams);
		if (qdhp.isValid()) {
			qsdhpDHParams = qdhp;
		} else {
			log(QString::fromLatin1("Unable to use specified Diffie-Hellman parameters (sslDHParams): %1")
					.arg(qdhp.errorString()));
		}
	}
#else
	if (!dhparams.isEmpty()) {
		log("Diffie-Hellman parameters (sslDHParams) were specified, but will not be used. This version of Murmur does "
			"not support Diffie-Hellman parameters.");
	}
#endif

	QString issuer;

	QStringList issuerNames = qscCert.issuerInfo(QSslCertificate::CommonName);
	if (!issuerNames.isEmpty()) {
		issuer = issuerNames.first();
	}

	// Really old certs/keys are no good, throw them away so we can
	// generate a new one below.
	if (issuer == QString::fromUtf8("Murmur Autogenerated Certificate")) {
		log("Old autogenerated certificate is unusable for registration, invalidating it");
		qscCert = QSslCertificate();
		qskKey  = QSslKey();
	}

	// If we have a cert, and it's a self-signed one, but we're binding to
	// all the same addresses as the Meta server is, use it's cert instead.
	// This allows a self-signed certificate generated by Murmur to be
	// replaced by a CA-signed certificate in the .ini file.
	if (!qscCert.isNull() && issuer.startsWith(QString::fromUtf8("Murmur Autogenerated Certificate"))
		&& !Meta::mp->qscCert.isNull() && !Meta::mp->qskKey.isNull() && (Meta::mp->qlBind == qlBind)) {
		qscCert         = Meta::mp->qscCert;
		qskKey          = Meta::mp->qskKey;
		qlIntermediates = Meta::mp->qlIntermediates;

		if (!qscCert.isNull() && !qskKey.isNull()) {
			bUsingMetaCert = true;
		}
	}

	// If we still don't have a certificate by now, try to load the one from Meta
	if (qscCert.isNull() || qskKey.isNull()) {
		if (!key.isEmpty() || !crt.isEmpty()) {
			log("Certificate specified, but failed to load.");
		}

		qskKey          = Meta::mp->qskKey;
		qscCert         = Meta::mp->qscCert;
		qlIntermediates = Meta::mp->qlIntermediates;

		if (!qscCert.isNull() && !qskKey.isNull()) {
			bUsingMetaCert = true;
		}

		// If loading from Meta doesn't work, build+sign a new one
		if (qscCert.isNull() || qskKey.isNull()) {
			log("Generating new server certificate.");

			if (!SelfSignedCertificate::generateMurmurV2Certificate(qscCert, qskKey)) {
				log("Certificate or key generation failed");
			}

			m_dbWrapper.setConfiguration(iServerNum, "certificate", qscCert.toPem().toStdString());
			m_dbWrapper.setConfiguration(iServerNum, "key", qskKey.toPem().toStdString());
		}
	}

	// Drain OpenSSL's per-thread error queue
	// to ensure that errors from the operations
	// we've done in here do not leak out into
	// Qt's SSL module.
	//
	// If an error leaks, it can break all connections
	// to the server because each invocation of Qt's SSL
	// read callback checks OpenSSL's per-thread error
	// queue (albeit indirectly, via SSL_get_error()).
	// Qt expects any errors returned from SSL_get_error()
	// to be related to the QSslSocket it is currently
	// processing -- which is the obvious thing to expect:
	// SSL_get_error() takes a pointer to an SSL object
	// and the return code of the failed operation.
	// However, it is also documented as:
	//
	//  "In addition to ssl and ret, SSL_get_error()
	//   inspects the current thread's OpenSSL error
	//   queue."
	//
	// So, if any OpenSSL operation on the main thread
	// forgets to clear the error queue, those errors
	// *will* leak into other things that *do* error
	// checking. In our case, into Qt's SSL read callback,
	// resulting in all clients being disconnected.
	ERR_clear_error();
}

const QString Server::getDigest() const {
	return QString::fromLatin1(qscCert.digest(QCryptographicHash::Sha1).toHex());
}
