/*
 * radius.c	Functions to send/receive radius packets.
 *
 * Version:	@(#)radius.c  2.35  19-Jul-1999  miquels@cistron.nl
 *
 */

char radius_sccsid[] =
"@(#)radius.c 	2.35 Copyright 1998-1999 Cistron Internet Services B.V.";

#include	"autoconf.h"

#include	<sys/types.h>
#include	<sys/time.h>
#include	<netinet/in.h>
#include	<arpa/inet.h>

#include	<stdio.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<string.h>
#include	<time.h>
#include	<ctype.h>
#include	<errno.h>

#include	"libradius.h"

#if HAVE_MALLOC_H
#  include	<malloc.h>
#endif

/*
 *  ??? Why this number?  The RFC says 4096 octets max,
 *  and most packets are less than 256.
 */
#define PACKET_DATA_LEN 1600

typedef struct radius_packet_t {
  u_char	code;
  u_char	id;
  u_char	length[2];
  u_char	vector[16];
  u_char	data[1];
} radius_packet_t;

/*
 *	Reply to the request.  Also attach
 *	reply attribute value pairs and any user message provided.
 */
int rad_send(RADIUS_PACKET *packet, int activefd, char *secret)
{
	VALUE_PAIR		*reply;
	UINT4			lvalue;
	struct	sockaddr	saremote;
	struct	sockaddr_in	*sin;
	int			send_buffer[PACKET_DATA_LEN / sizeof(int)];
	int			len;
	int			secretlen;
	int			vendorcode, vendorpec;
	u_short			total_length, tmp;
	u_char			*ptr, *length_ptr;
	u_char			digest[16];
	char			*what;
	radius_packet_t		*hdr;

	hdr = (radius_packet_t *)send_buffer;
	reply = packet->vps;

	switch (packet->code) {
		case PW_PASSWORD_REJECT:
		case PW_AUTHENTICATION_REJECT:
			what = "Reject";
			break;
		case PW_ACCESS_CHALLENGE:
			what = "Challenge";
			break;
		case PW_AUTHENTICATION_ACK:
			what = "Ack";
			break;
		case PW_ACCOUNTING_RESPONSE:
			what = "Accounting Ack";
			break;
		case PW_AUTHENTICATION_REQUEST:
			what = "Authentication request";
			break;
		case PW_ACCOUNTING_REQUEST:
			what = "Accounting request";
			break;
		default:
			what = "Reply";
			break;
	}

	/*
	 *	Build standard header
	 */
	hdr->code = packet->code;
	hdr->id = packet->id;
	if (packet->code == PW_ACCOUNTING_REQUEST)
		memset(hdr->vector, 0, AUTH_VECTOR_LEN);
	else
		memcpy(hdr->vector, packet->vector, AUTH_VECTOR_LEN);

	DEBUG("Sending %s of id %d to %s\n",
		what, packet->id,
		inet_ntoa(*(struct in_addr *)&packet->dst_ipaddr));

	total_length = AUTH_HDR_LEN;

	/*
	 *	Load up the configuration values for the user
	 */
	ptr = hdr->data;
	while (reply != NULL) {
		debug_pair(reply);

		/*
		 *	This could be a vendor-specific attribute.
		 */
		length_ptr = NULL;
		if ((vendorcode = VENDOR(reply->attribute)) > 0 &&
		    (vendorpec  = dict_vendorpec(vendorcode)) > 0) {
			*ptr++ = PW_VENDOR_SPECIFIC;
			length_ptr = ptr;
			*ptr++ = 6;
			lvalue = htonl(vendorpec);
			memcpy(ptr, &lvalue, 4);
			ptr += 4;
			total_length += 6;
		} else if (reply->attribute > 0xff) {
			/*
			 *	Ignore attributes > 0xff
			 */
			reply = reply->next;
			continue;
		} else
			vendorpec = 0;

#ifdef ATTRIB_NMC
		if (vendorpec == VENDORPEC_USR) {
			lvalue = htonl(reply->attribute & 0xFFFF);
			memcpy(ptr, &lvalue, 4);
			total_length += 2;
			*length_ptr  += 2;
			ptr          += 4;
		} else
#endif
		*ptr++ = (reply->attribute & 0xFF);

		switch(reply->type) {

		case PW_TYPE_STRING:
			/*
			 *	FIXME: this is just to make sure but
			 *	should NOT be needed. In fact I have no
			 *	idea if it is needed :)
			 */
			if (reply->length == 0 && reply->strvalue[0] != 0)
				reply->length = strlen(reply->strvalue);

			len = reply->length;
			if (len >= MAX_STRING_LEN) {
				len = MAX_STRING_LEN - 1;
			}
#ifdef ATTRIB_NMC
			if (vendorpec != VENDORPEC_USR)
#endif
				*ptr++ = len + 2;
			if (length_ptr) *length_ptr += len + 2;
			memcpy(ptr, reply->strvalue,len);
			ptr += len;
			total_length += len + 2;
			break;

		case PW_TYPE_INTEGER:
		case PW_TYPE_IPADDR:
#ifdef ATTRIB_NMC
			if (vendorpec != VENDORPEC_USR)
#endif
				*ptr++ = sizeof(UINT4) + 2;
			if (length_ptr) *length_ptr += sizeof(UINT4)+ 2;
			if (reply->type != PW_TYPE_IPADDR)
				lvalue = htonl(reply->lvalue);
			else
				lvalue = reply->lvalue;
			memcpy(ptr, &lvalue, sizeof(UINT4));
			ptr += sizeof(UINT4);
			total_length += sizeof(UINT4) + 2;
			break;

		default:
			break;
		}

		reply = reply->next;
	}

	tmp = htons(total_length);
	memcpy(hdr->length, &tmp, sizeof(u_short));

	/*
	 *	If this is not an authentication request, we
	 *	need to calculate the md5 hash over the entire packet
	 *	and put it in the vector.
	 */
	if (packet->code != PW_AUTHENTICATION_REQUEST) {
		secretlen = strlen(secret);
		memcpy((char *)send_buffer + total_length, secret, secretlen);
		librad_md5_calc(digest, (char *)send_buffer,
			total_length + secretlen);
		memcpy(hdr->vector, digest, AUTH_VECTOR_LEN);
		memset((char *)send_buffer + total_length, 0, secretlen);
	}

	/*
	 *	And send it on it's way.
	 */
	sin = (struct sockaddr_in *) &saremote;
        memset ((char *) sin, '\0', sizeof (saremote));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = packet->dst_ipaddr;
	sin->sin_port = htons(packet->dst_port);

	sendto(activefd, (char *)send_buffer, (int)total_length, 0,
			&saremote, sizeof(struct sockaddr_in));

	return 0;
}


/*
 *	Validates the requesting client NAS.  Calculates the
 *	signature based on the clients private key.
 */
int calc_acctdigest(RADIUS_PACKET *packet, char *secret, char *recvbuf, int len)
{
	int		secretlen;
	char		digest[AUTH_VECTOR_LEN];

	/*
	 *	Older clients have the authentication vector set to
	 *	all zeros. Return `1' in that case.
	 */
	memset(digest, 0, sizeof(digest));
	if (memcmp(packet->vector, digest, AUTH_VECTOR_LEN) == 0) {
		packet->verified = 1;
		return 1;
	}

	/*
	 *	Zero out the auth_vector in the received packet.
	 *	Then append the shared secret to the received packet,
	 *	and calculate the MD5 sum. This must be the same
	 *	as the original MD5 sum (packet->vector).
	 */
	secretlen = strlen(secret);
	memset(recvbuf + 4, 0, AUTH_VECTOR_LEN);
	memcpy(recvbuf + len, secret, secretlen);
	librad_md5_calc(digest, recvbuf, len + secretlen);

	/*
	 *	Return 0 if OK, 2 if not OK.
	 */
	packet->verified =
	memcmp(digest, packet->vector, AUTH_VECTOR_LEN) ? 2 : 0;

	return packet->verified;
}

/*
 *	Receive UDP client requests, and fill in
 *	the basics of a RADIUS_PACKET structure.
 */
RADIUS_PACKET *rad_recv(int fd)
{
	RADIUS_PACKET		*packet;
	struct sockaddr_in	saremote;
	int			totallen;
	int			salen;
	u_short			len;
	u_char			*attr;
	int			count;
	radius_packet_t		*hdr;

	/*
	 *	Allocate the new request data structure
	 */
	if ((packet = malloc(sizeof(RADIUS_PACKET))) == NULL) {
		librad_log("out of memory");
		errno = ENOMEM;
		return NULL;
	}
	memset(packet, 0, sizeof(RADIUS_PACKET));
	if ((packet->data = malloc(PACKET_DATA_LEN)) == NULL) {
		free(packet);
		librad_log("out of memory");
		errno = ENOMEM;
		return NULL;
	}

	/*
	 *	Receive the packet.
	 */
	salen = sizeof(saremote);
	packet->data_len = recvfrom(fd, packet->data, PACKET_DATA_LEN,
		0, (struct sockaddr *)&saremote, &salen);

	/*
	 *	Check for socket errors.
	 */
	if (packet->data_len < 0) {
		free(packet->data);
		free(packet);
		librad_log("Error receiving packet: %s", strerror(errno));
		return NULL;
	}

	/*
	 *	Check for packets smaller than the packet header.
	 */
	if (packet->data_len < AUTH_HDR_LEN) {
		free(packet->data);
		free(packet);
		librad_log("Malformed RADIUS packet: too small");
		return NULL;
	}

	/*
	 *	Check for packets with mismatched size.
	 *	i.e. We've received 128 bytes, and the packet header
	 *	says it's 256 bytes long.
	 */
	hdr = (radius_packet_t *)packet->data;
	memcpy(&len, hdr->length, sizeof(u_short));
	totallen = ntohs(len);
	if (packet->data_len != totallen) {
		librad_log("Malformed RADIUS packet: received %d octets, packet size says %d", packet->data_len, totallen);
		free(packet->data);
		free(packet);
		return NULL;
	}

	/*
	 *	Walk through the packet's attributes, ensuring that
	 *	they add up EXACTLY to the size of the packet.
	 *
	 *	If they don't, then the attributes either under-fill
	 *	or over-fill the packet.  Any parsing of the packet
	 *	is impossible, and will result in unknown side effects.
	 *
	 *	This would ONLY happen with buggy RADIUS implementations,
	 *	or with an intentional attack.  Either way, we do NOT want
	 *	to be vulnerable to this problem.
	 */
	attr = hdr->data;
	count = totallen - AUTH_HDR_LEN;
	while (count > 0) {
	  count -= attr[1];	/* grab the attribute length */
	  attr += attr[1];
	}

	/*
	 *	If the attributes add up to a packet, it's allowed.
	 *
	 *	If not, we complain, and throw the packet away.
	 */
	if (count != 0) {
		librad_log("Malformed RADIUS packet: packet attributes do NOT exactly fill the packet");
		free(packet->data);
		free(packet);
		return NULL;	}

	DEBUG("rad_recv: Packet from host %s code=%d, id=%d, length=%d\n",
			inet_ntoa(saremote.sin_addr),
			hdr->code, hdr->id, totallen);

	/*
	 *	Fill header fields
	 */
	packet->src_ipaddr = saremote.sin_addr.s_addr;
	packet->src_port = ntohs(saremote.sin_port);
	packet->code = hdr->code;
	packet->id = hdr->id;
	memcpy(packet->vector, hdr->vector, AUTH_VECTOR_LEN);

	return packet;
}

/*
 *	Calculate/check digest, and decode radius attributes.
 */
int rad_decode(RADIUS_PACKET *packet, char *secret)
{
	DICT_ATTR		*attr;
	UINT4			lvalue;
	UINT4			vendorcode;
	UINT4			vendorpec;
	VALUE_PAIR		*first_pair;
	VALUE_PAIR		*prev;
	VALUE_PAIR		*pair;
	u_char			*ptr;
	int			length;
	int			attribute;
	int			attrlen;
	int			vendorlen;
	radius_packet_t		*hdr;

	hdr = (radius_packet_t *)packet->data;
	length = packet->data_len;

	/*
	 *	Calculate and/or verify digest.
	 */
	switch(packet->code) {
		case PW_AUTHENTICATION_REQUEST:
			break;
		case PW_ACCOUNTING_REQUEST:
			if (calc_acctdigest(packet, secret,
			    packet->data, length) > 1) {
				librad_log("Received accounting packet "
				    "from %s with invalid signature!",
				    inet_ntoa(*(struct in_addr*)&(packet->src_ipaddr)));
				return 1;
			}
			break;
		case PW_AUTHENTICATION_ACK:
		case PW_AUTHENTICATION_REJECT:
		case PW_ACCOUNTING_RESPONSE:
			/*
			 *	Answers from remote radius servers.
			 *	Need to verify the answer.
			 *	FIXME: actually do that here !!!
			 */
			break;
	}

	/*
	 *	Extract attribute-value pairs
	 */
	ptr = hdr->data;
	length -= AUTH_HDR_LEN;
	first_pair = NULL;
	prev = NULL;

	vendorcode = 0;
	vendorlen  = 0;

	while(length > 0) {

		if (vendorlen > 0) {
			attribute = *ptr++ | (vendorcode << 16);
			attrlen   = *ptr++;
		} else {
			attribute = *ptr++;
			attrlen   = *ptr++;
		}
		if (attrlen < 2) {
			length = 0;
			continue;
		}
		attrlen -= 2;
		length  -= 2;

		/*
		 *	This could be a Vendor-Specific attribute.
		 *
		 */
		if (vendorlen <= 0 &&
		    attribute == PW_VENDOR_SPECIFIC && attrlen > 6) {
			memcpy(&lvalue, ptr, 4);
			vendorpec = ntohl(lvalue);
			if ((vendorcode = dict_vendorcode(vendorpec))
			    != 0) {
#ifdef ATTRIB_NMC
				if (vendorpec == VENDORPEC_USR) {
					ptr += 4;
					memcpy(&lvalue, ptr, 4);
					/*printf("received USR %04x\n", ntohl(lvalue));*/
					attribute = (ntohl(lvalue) & 0xFFFF) |
							(vendorcode << 16);
					ptr += 4;
					attrlen -= 8;
					length -= 8;
				} else
#endif
				{
					ptr += 4;
					vendorlen = attrlen - 4;
					attribute = *ptr++ | (vendorcode << 16);
					attrlen   = *ptr++;
					attrlen -= 2;
					length -= 6;
				}
			}
		}

		if ( attrlen >= MAX_STRING_LEN ) {
			DEBUG("attribute %d too long, %d >= %d\n", attribute,
				attrlen, MAX_STRING_LEN);
		}
		else if ( attrlen > length ) {
			DEBUG("attribute %d longer as buffer left, %d > %d\n",
				attribute, attrlen, length);
		}
		else {
			/*
			 *	FIXME: should we us paircreate() ?
			 */
			if ((pair = malloc(sizeof(VALUE_PAIR))) == NULL) {
				pairfree(first_pair);
				librad_log("out of memory");
				errno = ENOMEM;
				return -1;
			}
			memset(pair, 0, sizeof(VALUE_PAIR));
			if ((attr = dict_attrbyvalue(attribute)) == NULL) {
				sprintf(pair->name, "Attr-%d", attribute);
				pair->type = PW_TYPE_STRING;
			} else {
				strcpy(pair->name, attr->name);
				pair->type = attr->type;
			}
			pair->attribute = attribute;
			pair->length = attrlen;
			pair->next = NULL;

			switch (attr->type) {

			case PW_TYPE_STRING:
				/* attrlen always < MAX_STRING_LEN */
				memcpy(pair->strvalue, ptr, attrlen);
				debug_pair(pair);
				if (first_pair == NULL)
					first_pair = pair;
				else
					prev->next = pair;
				prev = pair;
				break;
			
			case PW_TYPE_INTEGER:
			case PW_TYPE_DATE:
			case PW_TYPE_IPADDR:
				memcpy(&lvalue, ptr, sizeof(UINT4));
				if (attr->type != PW_TYPE_IPADDR)
					pair->lvalue = ntohl(lvalue);
				else
					pair->lvalue = lvalue;
				debug_pair(pair);
				if (first_pair == NULL)
					first_pair = pair;
				else
					prev->next = pair;
				prev = pair;
				break;
			
			default:
				DEBUG("    %s (Unknown Type %d)\n",
					attr->name,attr->type);
				free(pair);
				break;
			}

		}
		ptr += attrlen;
		length -= attrlen;
		if (vendorlen > 0) vendorlen -= (attrlen + 2);
	}

	packet->vps = first_pair;

	free(packet->data);
	packet->data = NULL;
	packet->data_len = 0;

	return 0;
}


/*
 *	Encode password.
 *
 *	We assume that the passwd buffer passed is big enough.
 *	RFC2138 says the password is max 128 chars, so the size
 *	of the passwd buffer must be at least 129 characters.
 *	Preferably it's just MAX_STRING_LEN.
 *
 *	int *pwlen is updated to the new length of the encrypted
 *	password - a multiple of 16 bytes.
 */
int rad_pwencode(char *passwd, int *pwlen, char *secret, char *vector)
{
	u_char	buffer[AUTH_VECTOR_LEN + MAX_STRING_LEN + 1];
	char	digest[AUTH_VECTOR_LEN];
	int	i, n, secretlen;
	int	len;

	/*
	 *	Padd password to multiple of 16 bytes.
	 */
	len = strlen(passwd);
	if (len > 128) len = 128;
	*pwlen = len;
	if (len % 16 != 0) {
		n = 16 - (len % 16);
		for (i = len; n > 0; n--, i++)
			passwd[i] = 0;
		len = *pwlen = i;
	}

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	strcpy(buffer, secret);
	memcpy(buffer + secretlen, vector, AUTH_VECTOR_LEN);
	librad_md5_calc(digest, buffer, secretlen + AUTH_VECTOR_LEN);

	/*
	 *	Now we can encode the password *in place*
	 */
	for (i = 0; i < 16; i++)
		passwd[i] ^= digest[i];

	if (len <= 16) return 0;

	/*
	 *	Length > 16, so we need to use the extended
	 *	algorithm.
	 */
	for (n = 0; n < 128 && n <= (len - 16); n += 16) { 
		memcpy(buffer + secretlen, passwd + n, 16);
		librad_md5_calc(digest, buffer, secretlen + 16);
		for (i = 0; i < 16; i++)
			passwd[i + n + 16] ^= digest[i];
	}

	return 0;
}

/*
 *	Decode password.
 */
int rad_pwdecode(char *passwd, int pwlen, char *secret, char *vector)
{
	u_char	buffer[AUTH_VECTOR_LEN + MAX_STRING_LEN + 1];
	char	digest[AUTH_VECTOR_LEN];
	char	r[AUTH_VECTOR_LEN];
	char	*s;
	int	i, n, secretlen;
	int	rlen;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	strcpy(buffer, secret);
	memcpy(buffer + secretlen, vector, AUTH_VECTOR_LEN);
	librad_md5_calc(digest, buffer, secretlen + AUTH_VECTOR_LEN);

	/*
	 *	Now we can decode the password *in place*
	 */
	memcpy(r, passwd, 16);
	for (i = 0; i < 16 && i < pwlen; i++)
		passwd[i] ^= digest[i];

	if (pwlen <= 16) {
		passwd[i] = 0;
		return 0;
	}

	/*
	 *	Length > 16, so we need to use the extended
	 *	algorithm.
	 */
	rlen = ((pwlen - 1) / 16) * 16;

	for (n = rlen; n > 0; n -= 16 ) { 
		s = (n == 16) ? r : (passwd + n - 16);
		memcpy(buffer + secretlen, s, 16);
		librad_md5_calc(digest, buffer, secretlen + 16);
		for (i = 0; i < 16 && (i + n) < pwlen; i++)
			passwd[i + n] ^= digest[i];
	}
	passwd[pwlen] = 0;

	return 0;
}

/*
 *	Create a random vector of AUTH_VECTOR_LEN bytes.
 */
static void random_vector(char *vector)
{
	int		randno;
	int		i;
	static int	did_srand = 0;
#ifdef __linux__
	static int	urandom_fd = -1;
#endif

#ifdef __linux__
	/*
	 *	Use /dev/urandom if available.
	 */
	if (urandom_fd > -2) {
		/*
		 *	Open urandom fd if not yet opened.
		 */
		if (urandom_fd < 0)
			urandom_fd = open("/dev/urandom", O_RDONLY);
		if (urandom_fd < 0) {
			/*
			 *	It's not there, don't try
			 *	it again.
			 */
			DEBUG("Cannot open /dev/urandom, using rand()\n");
			urandom_fd = -2;
		} else {

			fcntl(urandom_fd, F_SETFD, 1);

			/*
			 *	Read 16 bytes.
			 */
			if (read(urandom_fd, vector, AUTH_VECTOR_LEN)
			    == AUTH_VECTOR_LEN)
				return;
			/*
			 *	We didn't get 16 bytes - fall
			 *	back on rand) and don't try again.
			 */
		DEBUG("Read short packet from /dev/urandom, using rand()\n");
			urandom_fd = -2;
		}
	}
#endif

	if (!did_srand) {
		srand(time(0) + getpid());
		did_srand = 1;
	}
	for (i = 0; i < AUTH_VECTOR_LEN; i += sizeof(int)) {
		randno = rand();
		memcpy(vector, &randno, sizeof(int));
		vector += sizeof(int);
	}
}


/*
 *	Allocate a new RADIUS_PACKET
 */
RADIUS_PACKET *rad_alloc(int newvector)
{
	RADIUS_PACKET	*rp;

	if ((rp = malloc(sizeof(RADIUS_PACKET))) == NULL)
		return NULL;
	memset(rp, 0, sizeof(RADIUS_PACKET));
	if (newvector)
		random_vector(rp->vector);

	return rp;
}

/*
 *	Free a RADIUS_PACKET
 */
void rad_free(RADIUS_PACKET *rp)
{
	if (rp->data) free(rp->data);
	if (rp->vps) pairfree(rp->vps);
	free(rp);
}
